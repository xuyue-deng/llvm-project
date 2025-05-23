get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(extension_libs GLOBAL PROPERTY MLIR_EXTENSION_LIBS)

set(LLVM_LINK_COMPONENTS
  TargetParser
)

set(LIBS
  CUFDialect
  FIRBuilder
  FIRCodeGenDialect
  FIRDialect
  FIRDialectSupport
  FIRSupport
  HLFIRDialect
)

add_flang_unittest(FlangOptimizerTests
  Builder/CharacterTest.cpp
  Builder/ComplexTest.cpp
  Builder/DoLoopHelperTest.cpp
  Builder/FIRBuilderTest.cpp
  Builder/HLFIRToolsTest.cpp
  Builder/Runtime/AllocatableTest.cpp
  Builder/Runtime/AssignTest.cpp
  Builder/Runtime/CommandTest.cpp
  Builder/Runtime/CharacterTest.cpp
  Builder/Runtime/DerivedTest.cpp
  Builder/Runtime/IntrinsicsTest.cpp
  Builder/Runtime/NumericTest.cpp
  Builder/Runtime/RaggedTest.cpp
  Builder/Runtime/ReductionTest.cpp
  Builder/Runtime/StopTest.cpp
  Builder/Runtime/TransformationalTest.cpp
  FIRContextTest.cpp
  FIRTypesTest.cpp
  FortranVariableTest.cpp
  InternalNamesTest.cpp
  KindMappingTest.cpp
  RTBuilder.cpp
DEPENDS
  CUFDialect
  FIRDialect
  FIRSupport
  HLFIRDialect)

if(NOT FLANG_STANDALONE_BUILD)
  add_dependencies(FlangOptimizerTests
    ${dialect_libs})
endif()

target_link_libraries(FlangOptimizerTests
  PRIVATE
  ${LIBS})
mlir_target_link_libraries(FlangOptimizerTests
  PRIVATE
  ${dialect_libs}
  ${extension_libs}
  )
