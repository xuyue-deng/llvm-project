//===-- lib/Semantics/check-coarray.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "check-coarray.h"
#include "definable.h"
#include "flang/Common/indirection.h"
#include "flang/Evaluate/expression.h"
#include "flang/Parser/message.h"
#include "flang/Parser/parse-tree.h"
#include "flang/Parser/tools.h"
#include "flang/Semantics/expression.h"
#include "flang/Semantics/tools.h"

namespace Fortran::semantics {

class CriticalBodyEnforce {
public:
  CriticalBodyEnforce(
      SemanticsContext &context, parser::CharBlock criticalSourcePosition)
      : context_{context}, criticalSourcePosition_{criticalSourcePosition} {}
  std::set<parser::Label> labels() { return labels_; }
  template <typename T> bool Pre(const T &) { return true; }
  template <typename T> void Post(const T &) {}

  template <typename T> bool Pre(const parser::Statement<T> &statement) {
    currentStatementSourcePosition_ = statement.source;
    if (statement.label.has_value()) {
      labels_.insert(*statement.label);
    }
    return true;
  }

  // C1118
  void Post(const parser::ReturnStmt &) {
    context_
        .Say(currentStatementSourcePosition_,
            "RETURN statement is not allowed in a CRITICAL construct"_err_en_US)
        .Attach(criticalSourcePosition_, GetEnclosingMsg());
  }
  void Post(const parser::ExecutableConstruct &construct) {
    if (IsImageControlStmt(construct)) {
      context_
          .Say(currentStatementSourcePosition_,
              "An image control statement is not allowed in a CRITICAL"
              " construct"_err_en_US)
          .Attach(criticalSourcePosition_, GetEnclosingMsg());
    }
  }

private:
  parser::MessageFixedText GetEnclosingMsg() {
    return "Enclosing CRITICAL statement"_en_US;
  }

  SemanticsContext &context_;
  std::set<parser::Label> labels_;
  parser::CharBlock currentStatementSourcePosition_;
  parser::CharBlock criticalSourcePosition_;
};

class ChangeTeamBodyEnforce {
public:
  ChangeTeamBodyEnforce(
      SemanticsContext &context, parser::CharBlock changeTeamSourcePosition)
      : context_{context}, changeTeamSourcePosition_{changeTeamSourcePosition} {
  }
  std::set<parser::Label> labels() { return labels_; }
  template <typename T> bool Pre(const T &) { return true; }
  template <typename T> void Post(const T &) {}

  template <typename T> bool Pre(const parser::Statement<T> &statement) {
    currentStatementSourcePosition_ = statement.source;
    if (statement.label.has_value()) {
      labels_.insert(*statement.label);
    }
    return true;
  }

  void Post(const parser::ReturnStmt &) {
    context_
        .Say(currentStatementSourcePosition_,
            "RETURN statement is not allowed in a CHANGE TEAM construct"_err_en_US)
        .Attach(
            changeTeamSourcePosition_, "Enclosing CHANGE TEAM construct"_en_US);
  }

private:
  SemanticsContext &context_;
  std::set<parser::Label> labels_;
  parser::CharBlock currentStatementSourcePosition_;
  parser::CharBlock changeTeamSourcePosition_;
};

template <typename T>
static void CheckTeamType(
    SemanticsContext &context, const T &x, bool mustBeVariable = false) {
  if (const auto *expr{GetExpr(context, x)}) {
    if (!IsTeamType(evaluate::GetDerivedTypeSpec(expr->GetType()))) {
      context.Say(parser::FindSourceLocation(x), // C1114
          "Team value must be of type TEAM_TYPE from module ISO_FORTRAN_ENV"_err_en_US);
    } else if (mustBeVariable && !IsVariable(*expr)) {
      context.Say(parser::FindSourceLocation(x),
          "Team must be a variable in this context"_err_en_US);
    }
  }
}

static void CheckTeamStat(
    SemanticsContext &context, const parser::ImageSelectorSpec::Stat &stat) {
  const parser::Variable &var{stat.v.thing.thing.value()};
  if (parser::GetCoindexedNamedObject(var)) {
    context.Say(parser::FindSourceLocation(var), // C931
        "Image selector STAT variable must not be a coindexed "
        "object"_err_en_US);
  }
}

static void CheckCoindexedStatOrErrmsg(SemanticsContext &context,
    const parser::StatOrErrmsg &statOrErrmsg, const std::string &listName) {
  auto CoindexedCheck{[&](const auto &statOrErrmsg) {
    if (const auto *expr{GetExpr(context, statOrErrmsg)}) {
      if (ExtractCoarrayRef(expr)) {
        context.Say(parser::FindSourceLocation(statOrErrmsg), // C1173
            "The stat-variable or errmsg-variable in a %s may not be a coindexed object"_err_en_US,
            listName);
      }
    }
  }};
  Fortran::common::visit(CoindexedCheck, statOrErrmsg.u);
}

static void CheckSyncStat(SemanticsContext &context,
    const parser::StatOrErrmsg &statOrErrmsg, bool &gotStat, bool &gotMsg) {
  common::visit(
      common::visitors{
          [&](const parser::StatVariable &stat) {
            if (gotStat) {
              context.Say( // C1172
                  "The stat-variable in a sync-stat-list may not be repeated"_err_en_US);
            }
            gotStat = true;
          },
          [&](const parser::MsgVariable &var) {
            WarnOnDeferredLengthCharacterScalar(context, GetExpr(context, var),
                var.v.thing.thing.GetSource(), "ERRMSG=");
            if (gotMsg) {
              context.Say( // C1172
                  "The errmsg-variable in a sync-stat-list may not be repeated"_err_en_US);
            }
            gotMsg = true;
          },
      },
      statOrErrmsg.u);

  CheckCoindexedStatOrErrmsg(context, statOrErrmsg, "sync-stat-list");
}

static void CheckSyncStatList(
    SemanticsContext &context, const std::list<parser::StatOrErrmsg> &list) {
  bool gotStat{false}, gotMsg{false};
  for (const parser::StatOrErrmsg &statOrErrmsg : list) {
    CheckSyncStat(context, statOrErrmsg, gotStat, gotMsg);
  }
}

static void CheckEventVariable(
    SemanticsContext &context, const parser::EventVariable &eventVar) {
  if (const auto *expr{GetExpr(context, eventVar)}) {
    if (!IsEventType(evaluate::GetDerivedTypeSpec(expr->GetType()))) { // C1176
      context.Say(parser::FindSourceLocation(eventVar),
          "The event-variable must be of type EVENT_TYPE from module ISO_FORTRAN_ENV"_err_en_US);
    }
  }
}

void CoarrayChecker::Leave(const parser::ChangeTeamStmt &x) {
  CheckNamesAreDistinct(std::get<std::list<parser::CoarrayAssociation>>(x.t));
  CheckTeamType(context_, std::get<parser::TeamValue>(x.t));
  CheckSyncStatList(context_, std::get<std::list<parser::StatOrErrmsg>>(x.t));
}

void CoarrayChecker::Leave(const parser::EndChangeTeamStmt &x) {
  CheckSyncStatList(context_, std::get<std::list<parser::StatOrErrmsg>>(x.t));
}

void CoarrayChecker::Leave(const parser::SyncAllStmt &x) {
  CheckSyncStatList(context_, x.v);
}

void CoarrayChecker::Leave(const parser::SyncImagesStmt &x) {
  CheckSyncStatList(context_, std::get<std::list<parser::StatOrErrmsg>>(x.t));
  const auto &imageSet{std::get<parser::SyncImagesStmt::ImageSet>(x.t)};
  if (const auto *intExpr{std::get_if<parser::IntExpr>(&imageSet.u)}) {
    if (const auto *expr{GetExpr(context_, *intExpr)}) {
      if (expr->Rank() > 1) {
        context_.Say(parser::FindSourceLocation(imageSet), // C1174
            "An image-set that is an int-expr must be a scalar or a rank-one array"_err_en_US);
      }
      if (const auto *someInt{
              std::get_if<evaluate::Expr<evaluate::SomeInteger>>(&expr->u)};
          someInt && evaluate::IsActuallyConstant(*someInt)) {
        auto converted{evaluate::Fold(context_.foldingContext(),
            evaluate::ConvertToType<evaluate::SubscriptInteger>(
                common::Clone(*someInt)))};
        if (const auto *cst{
                evaluate::UnwrapConstantValue<evaluate::SubscriptInteger>(
                    converted)}) {
          for (auto elt : cst->values()) {
            auto n{elt.ToInt64()};
            if (n < 1) {
              context_.Say(parser::FindSourceLocation(imageSet),
                  "Image number %jd in the image-set is not valid"_err_en_US,
                  std::intmax_t{n});
              break;
            }
          }
        }
      }
    }
  }
}

void CoarrayChecker::Leave(const parser::SyncMemoryStmt &x) {
  CheckSyncStatList(context_, x.v);
}

void CoarrayChecker::Leave(const parser::SyncTeamStmt &x) {
  CheckTeamType(context_, std::get<parser::TeamValue>(x.t));
  CheckSyncStatList(context_, std::get<std::list<parser::StatOrErrmsg>>(x.t));
}

static void CheckEventWaitSpecList(SemanticsContext &context,
    const std::list<parser::EventWaitSpec> &eventWaitSpecList) {
  bool gotStat{false}, gotMsg{false}, gotUntil{false};
  for (const parser::EventWaitSpec &eventWaitSpec : eventWaitSpecList) {
    common::visit(
        common::visitors{
            [&](const parser::ScalarIntExpr &untilCount) {
              if (gotUntil) {
                context.Say( // C1178
                    "Until-spec in a event-wait-spec-list may not be repeated"_err_en_US);
              }
              gotUntil = true;
            },
            [&](const parser::StatOrErrmsg &statOrErrmsg) {
              common::visit(
                  common::visitors{
                      [&](const parser::StatVariable &stat) {
                        if (gotStat) {
                          context.Say( // C1178
                              "A stat-variable in a event-wait-spec-list may not be repeated"_err_en_US);
                        }
                        gotStat = true;
                      },
                      [&](const parser::MsgVariable &var) {
                        WarnOnDeferredLengthCharacterScalar(context,
                            GetExpr(context, var),
                            var.v.thing.thing.GetSource(), "ERRMSG=");
                        if (gotMsg) {
                          context.Say( // C1178
                              "A errmsg-variable in a event-wait-spec-list may not be repeated"_err_en_US);
                        }
                        gotMsg = true;
                      },
                  },
                  statOrErrmsg.u);
              CheckCoindexedStatOrErrmsg(
                  context, statOrErrmsg, "event-wait-spec-list");
            },

        },
        eventWaitSpec.u);
  }
}

void CoarrayChecker::Leave(const parser::NotifyWaitStmt &x) {
  const auto &notifyVar{std::get<parser::Scalar<parser::Variable>>(x.t)};

  if (const auto *expr{GetExpr(context_, notifyVar)}) {
    if (ExtractCoarrayRef(expr)) {
      context_.Say(parser::FindSourceLocation(notifyVar), // F2023 - C1178
          "A notify-variable in a NOTIFY WAIT statement may not be a coindexed object"_err_en_US);
    } else if (!IsNotifyType(evaluate::GetDerivedTypeSpec(
                   expr->GetType()))) { // F2023 - C1177
      context_.Say(parser::FindSourceLocation(notifyVar),
          "The notify-variable must be of type NOTIFY_TYPE from module ISO_FORTRAN_ENV"_err_en_US);
    } else if (!evaluate::IsCoarray(*expr)) { // F2023 - C1612
      context_.Say(parser::FindSourceLocation(notifyVar),
          "The notify-variable must be a coarray"_err_en_US);
    }
  }

  CheckEventWaitSpecList(
      context_, std::get<std::list<parser::EventWaitSpec>>(x.t));
}

void CoarrayChecker::Leave(const parser::EventPostStmt &x) {
  CheckSyncStatList(context_, std::get<std::list<parser::StatOrErrmsg>>(x.t));
  CheckEventVariable(context_, std::get<parser::EventVariable>(x.t));
}

void CoarrayChecker::Leave(const parser::EventWaitStmt &x) {
  const auto &eventVar{std::get<parser::EventVariable>(x.t)};

  if (const auto *expr{GetExpr(context_, eventVar)}) {
    if (ExtractCoarrayRef(expr)) {
      context_.Say(parser::FindSourceLocation(eventVar), // C1177
          "A event-variable in a EVENT WAIT statement may not be a coindexed object"_err_en_US);
    } else {
      CheckEventVariable(context_, eventVar);
    }
  }

  CheckEventWaitSpecList(
      context_, std::get<std::list<parser::EventWaitSpec>>(x.t));
}

static void CheckLockVariable(
    SemanticsContext &context, const parser::LockVariable &lockVar) {
  if (const SomeExpr * expr{GetExpr(lockVar)}) {
    if (auto dyType{expr->GetType()}) {
      auto at{parser::FindSourceLocation(lockVar)};
      if (dyType->category() != TypeCategory::Derived ||
          dyType->IsUnlimitedPolymorphic() ||
          !IsLockType(&dyType->GetDerivedTypeSpec())) {
        context.Say(at,
            "Lock variable must have type LOCK_TYPE from ISO_FORTRAN_ENV"_err_en_US);
      } else if (auto whyNot{WhyNotDefinable(at, context.FindScope(at),
                     {DefinabilityFlag::DoNotNoteDefinition,
                         DefinabilityFlag::AllowEventLockOrNotifyType},
                     *expr)}) {
        whyNot->set_severity(parser::Severity::Because);
        context.Say(at, "Lock variable is not definable"_err_en_US)
            .Attach(std::move(*whyNot));
      }
    }
  }
}

void CoarrayChecker::Leave(const parser::LockStmt &x) {
  CheckLockVariable(context_, std::get<parser::LockVariable>(x.t));
  bool gotAcquired{false}, gotStat{false}, gotMsg{false};
  for (const parser::LockStmt::LockStat &lockStat :
      std::get<std::list<parser::LockStmt::LockStat>>(x.t)) {
    if (const auto *statOrErrmsg{
            std::get_if<parser::StatOrErrmsg>(&lockStat.u)}) {
      CheckSyncStat(context_, *statOrErrmsg, gotStat, gotMsg);
    } else {
      CHECK(std::holds_alternative<
          parser::Scalar<parser::Logical<parser::Variable>>>(lockStat.u));
      if (gotAcquired) {
        context_.Say(parser::FindSourceLocation(lockStat),
            "Multiple ACQUIRED_LOCK specifiers"_err_en_US);
      } else {
        gotAcquired = true;
      }
    }
  }
}

void CoarrayChecker::Leave(const parser::UnlockStmt &x) {
  CheckLockVariable(context_, std::get<parser::LockVariable>(x.t));
  CheckSyncStatList(context_, std::get<std::list<parser::StatOrErrmsg>>(x.t));
}

void CoarrayChecker::Leave(const parser::CriticalStmt &x) {
  CheckSyncStatList(context_, std::get<std::list<parser::StatOrErrmsg>>(x.t));
}

void CoarrayChecker::Leave(const parser::ImageSelector &imageSelector) {
  for (const auto &imageSelectorSpec :
      std::get<std::list<parser::ImageSelectorSpec>>(imageSelector.t)) {
    if (const auto *stat{std::get_if<parser::ImageSelectorSpec::Stat>(
            &imageSelectorSpec.u)}) {
      CheckTeamStat(context_, *stat);
    }
  }
}

void CoarrayChecker::Leave(const parser::FormTeamStmt &x) {
  CheckTeamType(
      context_, std::get<parser::TeamVariable>(x.t), /*mustBeVariable=*/true);
  for (const auto &spec :
      std::get<std::list<parser::FormTeamStmt::FormTeamSpec>>(x.t)) {
    if (const auto *statOrErrmsg{std::get_if<parser::StatOrErrmsg>(&spec.u)}) {
      CheckCoindexedStatOrErrmsg(
          context_, *statOrErrmsg, "form-team-spec-list");
    }
  }
}

void CoarrayChecker::Enter(const parser::CriticalConstruct &x) {
  auto &criticalStmt{std::get<parser::Statement<parser::CriticalStmt>>(x.t)};
  const parser::Block &block{std::get<parser::Block>(x.t)};
  CriticalBodyEnforce criticalBodyEnforce{context_, criticalStmt.source};
  parser::Walk(block, criticalBodyEnforce);
  parser::Walk(std::get<parser::Statement<parser::EndCriticalStmt>>(x.t),
      criticalBodyEnforce);
  LabelEnforce criticalLabelEnforce{
      context_, criticalBodyEnforce.labels(), criticalStmt.source, "CRITICAL"};
  parser::Walk(block, criticalLabelEnforce);
}

void CoarrayChecker::Enter(const parser::ChangeTeamConstruct &x) {
  auto &changeTeamStmt{
      std::get<parser::Statement<parser::ChangeTeamStmt>>(x.t)};
  const parser::Block &block{std::get<parser::Block>(x.t)};
  ChangeTeamBodyEnforce changeTeamBodyEnforce{context_, changeTeamStmt.source};
  parser::Walk(block, changeTeamBodyEnforce);
  parser::Walk(std::get<parser::Statement<parser::EndChangeTeamStmt>>(x.t),
      changeTeamBodyEnforce);
  LabelEnforce changeTeamLabelEnforce{context_, changeTeamBodyEnforce.labels(),
      changeTeamStmt.source, "CHANGE TEAM"};
  parser::Walk(block, changeTeamLabelEnforce);
}

// Check that coarray names and selector names are all distinct.
void CoarrayChecker::CheckNamesAreDistinct(
    const std::list<parser::CoarrayAssociation> &list) {
  std::set<parser::CharBlock> names;
  auto getPreviousUse{
      [&](const parser::Name &name) -> const parser::CharBlock * {
        auto pair{names.insert(name.source)};
        return !pair.second ? &*pair.first : nullptr;
      }};
  for (const auto &assoc : list) {
    const auto &decl{std::get<parser::CodimensionDecl>(assoc.t)};
    const auto &selector{std::get<parser::Selector>(assoc.t)};
    const auto &declName{std::get<parser::Name>(decl.t)};
    if (context_.HasError(declName)) {
      continue; // already reported an error about this name
    }
    if (auto *prev{getPreviousUse(declName)}) {
      Say2(declName.source, // C1113
          "Coarray '%s' was already used as a selector or coarray in this statement"_err_en_US,
          *prev, "Previous use of '%s'"_en_US);
    }
    // ResolveNames verified the selector is a simple name
    const parser::Name *name{parser::Unwrap<parser::Name>(selector)};
    if (name) {
      if (auto *prev{getPreviousUse(*name)}) {
        Say2(name->source, // C1113, C1115
            "Selector '%s' was already used as a selector or coarray in this statement"_err_en_US,
            *prev, "Previous use of '%s'"_en_US);
      }
    }
  }
}

void CoarrayChecker::Say2(const parser::CharBlock &name1,
    parser::MessageFixedText &&msg1, const parser::CharBlock &name2,
    parser::MessageFixedText &&msg2) {
  context_.Say(name1, std::move(msg1), name1)
      .Attach(name2, std::move(msg2), name2);
}
} // namespace Fortran::semantics
