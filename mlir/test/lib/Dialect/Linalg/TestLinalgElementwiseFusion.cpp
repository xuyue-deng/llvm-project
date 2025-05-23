//===- TestLinalgElementwiseFusion.cpp - Test Linalg elementwise fusion ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass for testing fusion of elementwise operations in
// Linalg, mainly linalg options.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;

static void addOperands(Operation *op, SetVector<Value> &operandSet) {
  if (!op)
    return;
  TypeSwitch<Operation *, void>(op)
      .Case<linalg::LinalgOp>([&](linalg::LinalgOp linalgOp) {
        SmallVector<Value> inputOperands = linalgOp.getDpsInputs();
        operandSet.insert_range(inputOperands);
      })
      .Default([&](Operation *operation) {
        operandSet.insert(operation->operand_begin(), operation->operand_end());
      });
}

template <int limit = 3>
static bool setFusedOpOperandLimit(OpOperand *fusedOperand) {
  Operation *producer = fusedOperand->get().getDefiningOp();
  if (!producer)
    return false;

  Operation *consumer = fusedOperand->getOwner();
  SetVector<Value> fusedOpOperands;
  if (producer->getNumResults() != 1)
    return false;
  addOperands(consumer, fusedOpOperands);
  fusedOpOperands.remove(producer->getResult(0));
  addOperands(producer, fusedOpOperands);
  return fusedOpOperands.size() <= limit;
}

namespace {

/// Pattern to test fusion of producer with consumer, even if producer has
/// multiple uses.
struct TestMultiUseProducerFusion : public OpRewritePattern<linalg::GenericOp> {
  using OpRewritePattern<linalg::GenericOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(linalg::GenericOp genericOp,
                                PatternRewriter &rewriter) const override {
    OpOperand *fusableOperand = nullptr;
    for (OpOperand &operand : genericOp->getOpOperands()) {
      if (linalg::areElementwiseOpsFusable(&operand)) {
        fusableOperand = &operand;
        break;
      }
    }
    if (!fusableOperand) {
      return rewriter.notifyMatchFailure(genericOp, "no fusable operand found");
    }
    std::optional<linalg::ElementwiseOpFusionResult> fusionResult =
        linalg::fuseElementwiseOps(rewriter, fusableOperand);
    if (!fusionResult)
      return rewriter.notifyMatchFailure(genericOp, "fusion failed");
    for (auto [origValue, replacement] : fusionResult->replacements) {
      rewriter.replaceUsesWithIf(origValue, replacement, [&](OpOperand &use) {
        return use.getOwner() != genericOp.getOperation();
      });
    }
    rewriter.eraseOp(genericOp);
    return success();
  }
};

struct TestLinalgElementwiseFusion
    : public PassWrapper<TestLinalgElementwiseFusion,
                         OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TestLinalgElementwiseFusion)

  TestLinalgElementwiseFusion() = default;
  TestLinalgElementwiseFusion(const TestLinalgElementwiseFusion &pass)
      : PassWrapper(pass) {}
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<affine::AffineDialect, linalg::LinalgDialect,
                    memref::MemRefDialect, tensor::TensorDialect>();
  }
  StringRef getArgument() const final {
    return "test-linalg-elementwise-fusion-patterns";
  }
  StringRef getDescription() const final {
    return "Test Linalg element wise operation fusion patterns";
  }

  Option<bool> fuseGenericOps{
      *this, "fuse-generic-ops",
      llvm::cl::desc("Test fusion of generic operations."),
      llvm::cl::init(false)};

  Option<bool> fuseGenericOpsControl{
      *this, "fuse-generic-ops-control",
      llvm::cl::desc(
          "Test fusion of generic operations with a control function."),
      llvm::cl::init(false)};

  Option<bool> fuseWithReshapeByExpansion{
      *this, "fuse-with-reshape-by-expansion",
      llvm::cl::desc(
          "Test fusion of generic operations with reshape by expansion"),
      llvm::cl::init(false)};

  Option<bool> controlFuseByExpansion{
      *this, "control-fusion-by-expansion",
      llvm::cl::desc(
          "Test controlling fusion of reshape with generic op by expansion"),
      llvm::cl::init(false)};

  Option<bool> fuseWithReshapeByCollapsing{
      *this, "fuse-with-reshape-by-collapsing",
      llvm::cl::desc("Test linalg expand_shape -> generic fusion patterns that "
                     "collapse the iteration space of the consumer"),
      llvm::cl::init(false)};

  Option<bool> fuseWithReshapeByCollapsingWithControlFn{
      *this, "fuse-with-reshape-by-collapsing-control",
      llvm::cl::desc("Test controlling the linalg expand_shape -> generic "
                     "fusion patterns that "
                     "collapse the iteration space of the consumer"),
      llvm::cl::init(false)};

  Option<bool> fuseMultiUseProducer{
      *this, "fuse-multiuse-producer",
      llvm::cl::desc("Test fusion of producer ops with multiple uses"),
      llvm::cl::init(false)};

  ListOption<int64_t> collapseDimensions{
      *this, "collapse-dimensions-control",
      llvm::cl::desc("Test controlling dimension collapse pattern")};

  void runOnOperation() override {
    MLIRContext *context = &this->getContext();
    func::FuncOp funcOp = this->getOperation();

    if (fuseGenericOps) {
      RewritePatternSet fusionPatterns(context);
      auto controlFn = [](OpOperand *operand) { return true; };
      linalg::populateElementwiseOpsFusionPatterns(fusionPatterns, controlFn);
      if (failed(applyPatternsGreedily(funcOp.getBody(),
                                       std::move(fusionPatterns))))
        return signalPassFailure();
      return;
    }

    if (fuseGenericOpsControl) {
      RewritePatternSet fusionPatterns(context);
      linalg::populateElementwiseOpsFusionPatterns(fusionPatterns,
                                                   setFusedOpOperandLimit<4>);

      if (failed(applyPatternsGreedily(funcOp.getBody(),
                                       std::move(fusionPatterns))))
        return signalPassFailure();
      return;
    }

    if (fuseWithReshapeByExpansion) {
      RewritePatternSet fusionPatterns(context);
      linalg::populateFoldReshapeOpsByExpansionPatterns(
          fusionPatterns, [](OpOperand * /*fusedOperand*/) { return true; });
      if (failed(applyPatternsGreedily(funcOp.getBody(),
                                       std::move(fusionPatterns))))
        return signalPassFailure();
      return;
    }

    if (controlFuseByExpansion) {
      RewritePatternSet fusionPatterns(context);

      linalg::ControlFusionFn controlReshapeFusionFn =
          [](OpOperand *fusedOperand) {
            Operation *producer = fusedOperand->get().getDefiningOp();
            if (!producer)
              return false;

            if (auto collapseOp = dyn_cast<tensor::CollapseShapeOp>(producer)) {
              if (!collapseOp.getSrc().getDefiningOp<linalg::LinalgOp>()) {
                return false;
              }
            }

            Operation *consumer = fusedOperand->getOwner();
            if (auto expandOp = dyn_cast<tensor::ExpandShapeOp>(consumer)) {
              if (expandOp->hasOneUse()) {
                OpOperand &use = *expandOp->getUses().begin();
                auto linalgOp = dyn_cast<linalg::LinalgOp>(use.getOwner());
                if (linalgOp && linalgOp.isDpsInit(&use))
                  return true;
              }
              return false;
            }
            return true;
          };

      linalg::populateFoldReshapeOpsByExpansionPatterns(fusionPatterns,
                                                        controlReshapeFusionFn);
      if (failed(applyPatternsGreedily(funcOp.getBody(),
                                       std::move(fusionPatterns))))
        return signalPassFailure();
      return;
    }

    if (fuseWithReshapeByCollapsing) {
      RewritePatternSet patterns(context);
      linalg::populateFoldReshapeOpsByCollapsingPatterns(
          patterns, [](OpOperand * /*fusedOperand */) { return true; });
      if (failed(applyPatternsGreedily(funcOp.getBody(), std::move(patterns))))
        return signalPassFailure();
      return;
    }

    if (fuseWithReshapeByCollapsingWithControlFn) {
      RewritePatternSet patterns(context);
      linalg::ControlFusionFn controlFn = [](OpOperand *fusedOperand) -> bool {
        Operation *producer = fusedOperand->get().getDefiningOp();
        if (isa<tensor::ExpandShapeOp>(producer)) {
          // Skip fusing the first operand.
          return fusedOperand->getOperandNumber();
        }
        Operation *consumer = fusedOperand->getOwner();
        if (auto collapseOp = dyn_cast<tensor::CollapseShapeOp>(consumer)) {
          auto producerResult = dyn_cast<OpResult>(collapseOp.getSrc());
          // skip fusing first result.
          return producerResult.getResultNumber();
        }
        return true;
      };
      linalg::populateFoldReshapeOpsByCollapsingPatterns(patterns, controlFn);
      if (failed(applyPatternsGreedily(funcOp.getBody(), std::move(patterns))))
        return signalPassFailure();
      return;
    }

    if (fuseMultiUseProducer) {
      RewritePatternSet patterns(context);
      patterns.insert<TestMultiUseProducerFusion>(context);
      if (failed(applyPatternsGreedily(funcOp.getBody(), std::move(patterns))))
        return signalPassFailure();
      return;
    }

    if (!collapseDimensions.empty()) {
      SmallVector<int64_t, 2> dims(collapseDimensions.begin(),
                                   collapseDimensions.end());
      linalg::GetCollapsableDimensionsFn collapseFn =
          [&dims](linalg::LinalgOp op) {
            SmallVector<ReassociationIndices> reassociations;
            reassociations.emplace_back(dims);
            return reassociations;
          };
      RewritePatternSet patterns(context);
      linalg::populateCollapseDimensions(patterns, collapseFn);
      if (failed(applyPatternsGreedily(funcOp.getBody(), std::move(patterns))))
        return signalPassFailure();
      return;
    }
  }
};

} // namespace

namespace mlir {
namespace test {
void registerTestLinalgElementwiseFusion() {
  PassRegistration<TestLinalgElementwiseFusion>();
}
} // namespace test
} // namespace mlir
