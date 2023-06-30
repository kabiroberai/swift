//===-------------------------- TestRunner.cpp ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines test::TestRunner, the pass responsible for running tests,
// specifically test::FunctionTest and test::ModuleTest (maybe someday).
//
// To see more about writing your own tests, see include/swift/SIL/Test.h.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Type.h"
#include "swift/Basic/TaggedUnion.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OSSALifetimeCompletion.h"
#include "swift/SIL/OwnershipLiveness.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/ParseTestSpecification.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgumentArrayRef.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBridging.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/ScopedAddressUtils.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Transforms/SimplifyCFG.h"
#include "swift/SILOptimizer/Utils/CanonicalizeBorrowScope.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <memory>

using namespace swift;
using namespace swift::test;

namespace swift::test {
class TestRunner : public SILFunctionTransform {
  void printTestLifetime(bool begin, unsigned testIndex, unsigned testCount,
                         StringRef name, ArrayRef<StringRef> components);
  void runTest(StringRef name, Arguments &arguments);
  void run() override;
  struct FunctionTestDependenciesImpl final
      : public test::FunctionTest::Dependencies {
    TestRunner *pass;
    SILFunction *function;
    FunctionTestDependenciesImpl(TestRunner *pass, SILFunction *function)
        : pass(pass), function(function) {}
    DominanceInfo *getDominanceInfo() override {
      auto *dominanceAnalysis = pass->getAnalysis<DominanceAnalysis>();
      return dominanceAnalysis->get(function);
    }
    SILPassManager *getPassManager() override { return pass->getPassManager(); }
    ~FunctionTestDependenciesImpl() {}
  };
};

void TestRunner::printTestLifetime(bool begin, unsigned testIndex,
                                   unsigned testCount, StringRef name,
                                   ArrayRef<StringRef> components) {
  StringRef word = begin ? "\nbegin" : "end";
  llvm::errs() << word << " running test " << testIndex + 1 << " of "
               << testCount << " on " << getFunction()->getName() << ": "
               << name << " with: ";
  for (unsigned long index = 0, size = components.size(); index < size;
       ++index) {
    auto componentString = components[index].trim();
    if (componentString.empty())
      continue;

    llvm::errs() << componentString;
    if (index != size - 1) {
      llvm::errs() << ", ";
    }
  }
  llvm::errs() << "\n";
}

void TestRunner::runTest(StringRef name, Arguments &arguments) {
  auto *test = FunctionTest::get(name);
  if (!test) {
    llvm::errs() << "No test named: " << name << "\n";
    assert(false && "Invalid test name");
  }
  auto *function = getFunction();
  FunctionTestDependenciesImpl dependencies(this, function);
  test->run(*function, arguments, *this, dependencies);
}

void TestRunner::run() {
  llvm::SmallVector<UnparsedSpecification, 2> testSpecifications;
  getTestSpecifications(getFunction(), testSpecifications);
  Arguments arguments;
  SmallVector<StringRef, 4> components;
  for (unsigned long index = 0, size = testSpecifications.size(); index < size;
       ++index) {
    components.clear();
    arguments.clear();
    auto testSpecification = testSpecifications[index];
    test::parseTestArgumentsFromSpecification(getFunction(), testSpecification,
                                              arguments, components);
    auto name = arguments.takeString();
    ArrayRef<StringRef> argumentStrings = components;
    argumentStrings = argumentStrings.drop_front();
    printTestLifetime(/*begin=*/true, /*index=*/index, /*size=*/size, name,
                      argumentStrings);
    runTest(name, arguments);
    printTestLifetime(/*begin=*/false, /*index=*/index, /*size=*/size, name,
                      argumentStrings);
  }
}

//===----------------------------------------------------------------------===//
// MARK: General Unit Tests
//===----------------------------------------------------------------------===//

// Arguments: NONE
// Dumps:
// - the function
static FunctionTest DumpFunctionTest("dump-function",
                                     [](auto &function, auto &, auto &) {
                                       function.dump();
                                     });

// Arguments: NONE
// Dumps: the index of the self argument of the current function
static FunctionTest FunctionGetSelfArgumentIndex(
    "function-get-self-argument-index", [](auto &function, auto &, auto &) {
      auto index = BridgedFunction{&function}.getSelfArgumentIndex();
      llvm::errs() << "self argument index = " << index << "\n";
    });

// Arguments:
// - string: list of characters, each of which specifies subsequent arguments
//           - A: (block) argument
//           - F: function
//           - B: block
//           - I: instruction
//           - V: value
//           - O: operand
//           - b: boolean
//           - u: unsigned
//           - s: string
// - ...
// - an argument of the type specified in the initial string
// - ...
// Dumps:
// - for each argument (after the initial string)
//   - its type
//   - something to identify the instance (mostly this means calling dump)
static FunctionTest TestSpecificationTest(
    "test-specification-parsing",
    [](auto &function, auto &arguments, auto &test) {
      auto expectedFields = arguments.takeString();
      for (auto expectedField : expectedFields) {
        switch (expectedField) {
        case 'A': {
          auto *argument = arguments.takeBlockArgument();
          llvm::errs() << "argument:\n";
          argument->dump();
          break;
        }
        case 'F': {
          auto *function = arguments.takeFunction();
          llvm::errs() << "function: " << function->getName() << "\n";
          break;
        }
        case 'B': {
          auto *block = arguments.takeBlock();
          llvm::errs() << "block:\n";
          block->dump();
          break;
        }
        case 'I': {
          auto *instruction = arguments.takeInstruction();
          llvm::errs() << "instruction: ";
          instruction->dump();
          break;
        }
        case 'V': {
          auto value = arguments.takeValue();
          llvm::errs() << "value: ";
          value->dump();
          break;
        }
        case 'O': {
          auto *operand = arguments.takeOperand();
          llvm::errs() << "operand: ";
          operand->print(llvm::errs());
          break;
        }
        case 'u': {
          auto u = arguments.takeUInt();
          llvm::errs() << "uint: " << u << "\n";
          break;
        }
        case 'b': {
          auto b = arguments.takeBool();
          llvm::errs() << "bool: " << b << "\n";
          break;
        }
        case 's': {
          auto s = arguments.takeString();
          llvm::errs() << "string: " << s << "\n";
          break;
        }
        default:
          llvm_unreachable("unknown field type was expected?!");
        }
      }
    });

//===----------------------------------------------------------------------===//
// MARK: OSSA Lifetime Unit Tests
//===----------------------------------------------------------------------===//

// Arguments:
// - value: entity whose fields' livenesses are being computed
// - string: "defs:"
// - variadic list of triples consisting of 
//   - value: a live-range defining value
//   - int: the beginning of the range of fields defined by the value
//   - int: the end of the range of the fields defined by the value
// - the string "uses:"
// - variadic list of quadruples consisting of
//   - instruction: a live-range user
//   - bool: whether the user is lifetime-ending
//   - int: the beginning of the range of fields used by the instruction
//   - int: the end of the range of fields used by the instruction
// Dumps:
// - the liveness result and boundary
//
// Computes liveness for the specified def nodes by considering the
// specified uses. The actual uses of the def nodes are ignored.
//
// This is useful for testing non-ssa liveness, for example, of memory
// locations. In that case, the def nodes may be stores and the uses may be
// destroy_addrs.
static FunctionTest FieldSensitiveMultiDefUseLiveRangeTest(
    "fieldsensitive-multidefuse-liverange",
    [](auto &function, auto &arguments, auto &test) {
      SmallVector<SILBasicBlock *, 8> discoveredBlocks;
      auto value = arguments.takeValue();
      FieldSensitiveMultiDefPrunedLiveRange liveness(&function, value,
                                                     &discoveredBlocks);

      llvm::outs() << "FieldSensitive MultiDef lifetime analysis:\n";
      if (arguments.takeString() != "defs:") {
        llvm::report_fatal_error(
            "test specification expects the 'defs:' label\n");
      }
      while (true) {
        auto argument = arguments.takeArgument();
        if (isa<StringArgument>(argument)) {
          if (cast<StringArgument>(argument).getValue() != "uses:") {
            llvm::report_fatal_error(
                "test specification expects the 'uses:' label\n");
          }
          break;
        }
        auto begin = arguments.takeUInt();
        auto end = arguments.takeUInt();
        TypeTreeLeafTypeRange range(begin, end);
        if (isa<InstructionArgument>(argument)) {
          auto *instruction = cast<InstructionArgument>(argument).getValue();
          llvm::outs() << "  def in range [" << begin << ", " << end
                       << ") instruction: " << *instruction;
          liveness.initializeDef(instruction, range);
          continue;
        }
        if (isa<ValueArgument>(argument)) {
          SILValue value = cast<ValueArgument>(argument).getValue();
          llvm::outs() << "  def in range [" << begin << ", " << end
                       << ") value: " << value;
          liveness.initializeDef(value, range);
          continue;
        }
        llvm::report_fatal_error(
            "test specification expects the 'uses:' label\n");
      }
      liveness.finishedInitializationOfDefs();
      while (arguments.hasUntaken()) {
        auto *inst = arguments.takeInstruction();
        auto lifetimeEnding = arguments.takeBool();
        auto begin = arguments.takeUInt();
        auto end = arguments.takeUInt();
        TypeTreeLeafTypeRange range(begin, end);
        liveness.updateForUse(inst, range, lifetimeEnding);
      }
      liveness.print(llvm::errs());

      FieldSensitivePrunedLivenessBoundary boundary(
          liveness.getNumSubElements());
      liveness.computeBoundary(boundary);
      boundary.print(llvm::errs());
    });

//===----------------------------------------------------------------------===//
// MARK: SimplifyCFG Unit Tests
//===----------------------------------------------------------------------===//

static FunctionTest SimplifyCFGCanonicalizeSwitchEnum(
    "simplify-cfg-canonicalize-switch-enum",
    [](auto &function, auto &arguments, auto &test) {
      auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
      passToRun->injectPassManager(test.getPassManager());
      passToRun->injectFunction(&function);
      SimplifyCFG(function, *passToRun, /*VerifyAll=*/false,
                  /*EnableJumpThread=*/false)
          .canonicalizeSwitchEnums();
    });

static FunctionTest SimplifyCFGSimplifySwitchEnumBlock(
    "simplify-cfg-simplify-switch-enum-block",
    [](auto &function, auto &arguments, auto &test) {
      auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
      passToRun->injectPassManager(test.getPassManager());
      passToRun->injectFunction(&function);
      SimplifyCFG(function, *passToRun, /*VerifyAll=*/false,
                  /*EnableJumpThread=*/false)
          .simplifySwitchEnumBlock(
              cast<SwitchEnumInst>(arguments.takeInstruction()));
    });

static FunctionTest SimplifyCFGSwitchEnumOnObjcClassOptional(
    "simplify-cfg-simplify-switch-enum-on-objc-class-optional",
    [](auto &function, auto &arguments, auto &test) {
      auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
      passToRun->injectPassManager(test.getPassManager());
      passToRun->injectFunction(&function);
      SimplifyCFG(function, *passToRun, /*VerifyAll=*/false,
                  /*EnableJumpThread=*/false)
          .simplifySwitchEnumOnObjcClassOptional(
              cast<SwitchEnumInst>(arguments.takeInstruction()));
    });

static FunctionTest SimplifyCFGSimplifySwitchEnumUnreachableBlocks(
    "simplify-cfg-simplify-switch-enum-unreachable-blocks",
    [](auto &function, auto &arguments, auto &test) {
      auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
      passToRun->injectPassManager(test.getPassManager());
      passToRun->injectFunction(&function);
      SimplifyCFG(function, *passToRun, /*VerifyAll=*/false,
                  /*EnableJumpThread=*/false)
          .simplifySwitchEnumUnreachableBlocks(
              cast<SwitchEnumInst>(arguments.takeInstruction()));
    });

static FunctionTest SimplifyCFGSimplifyTermWithIdenticalDestBlocks(
    "simplify-cfg-simplify-term-with-identical-dest-blocks",
    [](auto &function, auto &arguments, auto &test) {
      auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
      passToRun->injectPassManager(test.getPassManager());
      passToRun->injectFunction(&function);
      SimplifyCFG(function, *passToRun, /*VerifyAll=*/false,
                  /*EnableJumpThread=*/false)
          .simplifyTermWithIdenticalDestBlocks(arguments.takeBlock());
    });

static FunctionTest SimplifyCFGTryJumpThreading(
    "simplify-cfg-try-jump-threading",
    [](auto &function, auto &arguments, auto &test) {
      auto *passToRun = cast<SILFunctionTransform>(createSimplifyCFG());
      passToRun->injectPassManager(test.getPassManager());
      passToRun->injectFunction(&function);
      SimplifyCFG(function, *passToRun, /*VerifyAll=*/false,
                  /*EnableJumpThread=*/false)
          .tryJumpThreading(cast<BranchInst>(arguments.takeInstruction()));
    });

//===----------------------------------------------------------------------===//
// MARK: AccessPath Unit Tests
//===----------------------------------------------------------------------===//

struct AccessUseTestVisitor : public AccessUseVisitor {
  AccessUseTestVisitor()
    : AccessUseVisitor(AccessUseType::Overlapping,
                       NestedAccessType::IgnoreAccessBegin) {}

  bool visitUse(Operand *op, AccessUseType useTy) override {
    switch (useTy) {
    case AccessUseType::Exact:
      llvm::errs() << "Exact Use: ";
      break;
    case AccessUseType::Inner:
      llvm::errs() << "Inner Use: ";
      break;
    case AccessUseType::Overlapping:
      llvm::errs() << "Overlapping Use ";
      break;
    }
    llvm::errs() << *op->getUser();
    return true;
  }
};

static FunctionTest AccessPathBaseTest("accesspath-base", [](auto &function,
                                                             auto &arguments,
                                                             auto &test) {
  auto value = arguments.takeValue();
  function.dump();
  llvm::outs() << "Access path base: " << value;
  auto accessPathWithBase = AccessPathWithBase::compute(value);
  AccessUseTestVisitor visitor;
  visitAccessPathBaseUses(visitor, accessPathWithBase, &function);
});

} // namespace swift::test

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//

SILTransform *swift::createUnitTestRunner() { return new TestRunner(); }
