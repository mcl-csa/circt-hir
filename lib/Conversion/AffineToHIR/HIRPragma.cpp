//===- AffineToHIR.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Conversion/HIRPragma.h"
#include "../PassDetail.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HIR/IR/HIR.h"
#include "circt/Dialect/HIR/IR/HIRDialect.h"
#include "circt/Dialect/HIR/IR/helper.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Scheduling/Algorithms.h"
#include "circt/Scheduling/Problems.h"
#include "mlir/Dialect/Affine/Analysis/AffineAnalysis.h"
#include "mlir/Dialect/Affine/Analysis/AffineStructures.h"
#include "mlir/Dialect/Affine/IR/AffineMemoryOpInterfaces.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/Visitors.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"
#include <iostream>
#include <stack>

using namespace circt;
namespace {
struct HIRPragma : public HIRPragmaBase<HIRPragma> {
  void runOnOperation() override;

private:
  LogicalResult visitOp(mlir::func::FuncOp);
  LogicalResult visitOp(mlir::memref::AllocaOp);
  LogicalResult visitOp(mlir::AffineForOp);
  LogicalResult visitOp(mlir::AffineLoadOp);
  LogicalResult visitOp(mlir::func::CallOp);
  LogicalResult visitOp(mlir::arith::NegFOp);
  LogicalResult visitOp(mlir::LLVM::UndefOp);

private:
  llvm::DenseSet<mlir::func::FuncOp> setOfHWFunctions;
};

} // namespace

void HIRPragma::runOnOperation() {
  auto moduleOp = getOperation();
  OpBuilder builder(&moduleOp.getBodyRegion());
  moduleOp->setAttrs(builder.getDictionaryAttr(
      builder.getNamedAttr("hir.hls", builder.getUnitAttr())));
  llvm::DenseSet<Operation *> toErase;

  // Hoist all declarations and put the hw functions in a set.
  moduleOp->walk([this, &builder, &toErase](Operation *operation) {
    if (auto op = dyn_cast<mlir::func::FuncOp>(operation)) {
      if (op.isDeclaration()) {
        auto newDecl = builder.cloneWithoutRegions(op);
        this->setOfHWFunctions.insert(newDecl);
        toErase.insert(op);
      } else if (op.getName() == this->topLevelFuncName) {
        this->setOfHWFunctions.insert(op);
      } else {
        toErase.insert(op);
      }
    }
  });

  for (auto *op : toErase)
    op->erase();

  auto walkresult =
      moduleOp->walk<mlir::WalkOrder::PreOrder>([this](Operation *operation) {
        if (auto op = dyn_cast<mlir::func::FuncOp>(operation)) {
          if (failed(visitOp(op)))
            return WalkResult::interrupt();
        } else if (auto op = dyn_cast<mlir::memref::AllocaOp>(operation)) {
          if (failed(visitOp(op)))
            return WalkResult::interrupt();
        } else if (auto op = dyn_cast<mlir::AffineForOp>(operation)) {
          if (failed(visitOp(op)))
            return WalkResult::interrupt();
        } else if (auto op = dyn_cast<mlir::AffineLoadOp>(operation)) {
          if (failed(visitOp(op)))
            return WalkResult::interrupt();
        } else if (auto op = dyn_cast<mlir::func::CallOp>(operation)) {
          if (failed(visitOp(op)))
            return WalkResult::interrupt();
        } else if (auto op = dyn_cast<mlir::arith::NegFOp>(operation)) {
          if (failed(visitOp(op)))
            return WalkResult::interrupt();
        } else if (auto op = dyn_cast<mlir::LLVM::UndefOp>(operation)) {
          if (failed(visitOp(op)))
            return WalkResult::interrupt();
        }
        return WalkResult::advance();
      });
  if (walkresult.wasInterrupted()) {
    signalPassFailure();
  }
}

DictionaryAttr getHIRValueAttrs(DictionaryAttr hlsAttrs) {
  if (!hlsAttrs)
    return DictionaryAttr();
  Builder builder(hlsAttrs.getContext());
  SmallVector<NamedAttribute> attrs;

  int latency = 0;
  if (auto latencyAttr = hlsAttrs.getAs<IntegerAttr>("hls.INTERFACE_LATENCY"))
    latency = latencyAttr.getInt();
  attrs.push_back(
      builder.getNamedAttr("hir.delay", builder.getI64IntegerAttr(latency)));
  return builder.getDictionaryAttr(attrs);
}

Optional<DictionaryAttr> getHIRMemrefAttrs(Operation *operation,
                                           DictionaryAttr hlsAttrs,
                                           MemRefType ty) {
  if (!hlsAttrs)
    return DictionaryAttr();
  Builder builder(hlsAttrs.getContext());
  SmallVector<NamedAttribute> attrs;
  for (auto kv : hlsAttrs)
    if (kv.getName().str().substr(0, 3) != "hls")
      attrs.push_back(kv);

  ArrayAttr portsAttr;
  bool isReg = false;
  if (ty.getNumElements() == 1)
    isReg = true;
  else if (auto dim = hlsAttrs.getAs<IntegerAttr>("hls.ARRAY_PARTITION_DIM"))
    if (dim == 0)
      isReg = true;

  std::string type = "";
  Optional<NamedAttribute> rdPort;
  Optional<NamedAttribute> wrPort;
  if (isReg) {
    rdPort = builder.getNamedAttr("rd_latency", builder.getI64IntegerAttr(0));
    wrPort = builder.getNamedAttr("wr_latency", builder.getI64IntegerAttr(1));
    type = "ram_2p";
  } else if (auto ty = hlsAttrs.getAs<StringAttr>("hls.BIND_STORAGE_TYPE")) {
    auto rdLatency = hlsAttrs.getAs<IntegerAttr>("hls.BIND_STORAGE_RD_LATENCY");
    auto wrLatency = hlsAttrs.getAs<IntegerAttr>("hls.BIND_STORAGE_WR_LATENCY");
    if (rdLatency)
      rdPort = builder.getNamedAttr("rd_latency", rdLatency);
    if (wrLatency)
      wrPort = builder.getNamedAttr("wr_latency", wrLatency);
    type = ty.str();
  } else if (auto ty =
                 hlsAttrs.getAs<StringAttr>("hls.INTERFACE_STORAGE_TYPE")) {
    auto rdLatency = hlsAttrs.getAs<IntegerAttr>("hls.INTERFACE_RD_LATENCY");
    auto wrLatency = hlsAttrs.getAs<IntegerAttr>("hls.INTERFACE_WR_LATENCY");
    if (rdLatency)
      rdPort = builder.getNamedAttr("rd_latency", rdLatency);
    if (wrLatency)
      wrPort = builder.getNamedAttr("wr_latency", wrLatency);
    type = ty.str();
  } else {
    operation->emitError("Could not determine memory type.");
  }
  if (type == "ram_1p") {
    if (!rdPort && !wrPort) {
      operation->emitError("Could not determine rd and/or wr port latency. "
                           "Original attrs : ")
          << hlsAttrs;
      return llvm::None;
    }
    if (rdPort && wrPort)
      portsAttr =
          builder.getArrayAttr(builder.getDictionaryAttr({*rdPort, *wrPort}));
    else if (rdPort)
      portsAttr = builder.getArrayAttr(builder.getDictionaryAttr({*rdPort}));
    else
      portsAttr = builder.getArrayAttr(builder.getDictionaryAttr({*wrPort}));

  } else if (type == "ram_2p") {
    if (!rdPort || !wrPort) {
      operation->emitError(
          "Need both rd and wr port latency for simple dual port ram.");
      return llvm::None;
    }
    portsAttr = builder.getArrayAttr({builder.getDictionaryAttr(*rdPort),
                                      builder.getDictionaryAttr(*wrPort)});
  } else if (type == "ram_t2p") {
    if (!rdPort || !wrPort) {
      operation->emitError(
          "Need both rd and wr port latency for true dual port ram.");
      return llvm::None;
    }
    portsAttr =
        builder.getArrayAttr({builder.getDictionaryAttr({*rdPort, *wrPort}),
                              builder.getDictionaryAttr({*rdPort, *wrPort})});
  } else {
    operation->emitError("Unknown type of memory : ") << type;
  }

  attrs.push_back(builder.getNamedAttr("hir.memref.ports", portsAttr));

  if (isa<mlir::memref::AllocaOp>(operation)) {
    StringAttr memKind;
    if (isReg)
      memKind = builder.getStringAttr("reg");
    else if (auto impl = hlsAttrs.getAs<StringAttr>("hls.BIND_STORAGE_IMPL"))
      memKind = builder.getStringAttr(impl.strref().lower());
    attrs.push_back(builder.getNamedAttr("mem_kind", memKind));
  }

  return builder.getDictionaryAttr(attrs);
}

ArrayAttr newArgOrResultAttrs(Operation *op, ArrayAttr originalAttrs,
                              ArrayRef<Type> types) {
  if (!originalAttrs)
    return ArrayAttr();

  Builder builder(originalAttrs.getContext());
  SmallVector<Attribute> newArgAttrs;
  for (size_t i = 0; i < originalAttrs.size(); i++) {
    auto ty = types[i];
    auto argAttr = originalAttrs[i].dyn_cast<DictionaryAttr>();
    Optional<DictionaryAttr> newAttr;
    if (auto memTy = ty.dyn_cast<mlir::MemRefType>()) {
      newAttr = getHIRMemrefAttrs(op, argAttr, memTy);
    } else if (ty.isIntOrFloat()) {
      newAttr = getHIRValueAttrs(argAttr);
    } else {
      assert(false && "unreachable");
    }
    if (!newAttr)
      return op->emitError("Could not get memref attr for arg ") << i,
             ArrayAttr();
    newArgAttrs.push_back(*newAttr);
  }
  return builder.getArrayAttr(newArgAttrs);
}

LogicalResult HIRPragma::visitOp(mlir::func::FuncOp op) {
  Builder builder(op);
  ArrayAttr newArgAttr = newArgOrResultAttrs(
      op, op->getAttrOfType<ArrayAttr>("arg_attrs"), op.getArgumentTypes());
  if (newArgAttr)
    op->setAttr("arg_attrs", newArgAttr);
  ArrayAttr newResultAttr = newArgOrResultAttrs(
      op, op->getAttrOfType<ArrayAttr>("res_attrs"), op.getArgumentTypes());
  if (newResultAttr)
    op->setAttr("res_attrs", newResultAttr);
  op->setAttr("hwAccel", builder.getUnitAttr());
  return success();
}

LogicalResult HIRPragma::visitOp(mlir::memref::AllocaOp op) {
  auto newAttr =
      getHIRMemrefAttrs(op, op->getAttrDictionary(),
                        op.getMemref().getType().dyn_cast<MemRefType>());

  if (!newAttr)
    return failure();
  op->setAttrs(*newAttr);
  return success();
}

LogicalResult HIRPragma::visitOp(mlir::AffineForOp op) {
  auto iiAttr = op->getAttrOfType<IntegerAttr>("hls.PIPELINE_II");
  auto unrollAttr = op->getAttrOfType<IntegerAttr>("hls.UNROLL_FACTOR");
  if (iiAttr) {
    op->setAttr("II", iiAttr);
    op->removeAttr("hls.PIPELINE_II");
  }
  if (unrollAttr) {
    op->setAttr("UNROLL", unrollAttr);
    op->removeAttr("hls.UNROLL_FACTOR");
  }
  return success();
}

LogicalResult HIRPragma::visitOp(mlir::AffineLoadOp op) {
  auto memref = op.getMemref();
  IntegerAttr resultDelay;
  ArrayAttr portsAttr;
  if (auto *operation = memref.getDefiningOp()) {
    auto allocaOp = dyn_cast<mlir::memref::AllocaOp>(operation);
    assert(allocaOp);
    portsAttr = allocaOp->getAttrOfType<ArrayAttr>("hir.memref.ports");
  } else {
    auto funcOp =
        dyn_cast<mlir::func::FuncOp>(memref.getParentRegion()->getParentOp());
    assert(funcOp);
    size_t argNum;
    for (argNum = 0; argNum < memref.getParentRegion()->getNumArguments();
         argNum++)
      if (memref.getParentRegion()->getArgument(argNum) == memref)
        break;

    auto memrefAttrs = funcOp->getAttrOfType<ArrayAttr>("arg_attrs")[argNum]
                           .dyn_cast<DictionaryAttr>();
    if (memrefAttrs)
      portsAttr = memrefAttrs.getAs<ArrayAttr>("hir.memref.ports");
  }

  if (portsAttr)
    for (auto port : portsAttr)
      if (auto rdLatency = port.dyn_cast<DictionaryAttr>().getAs<IntegerAttr>(
              "rd_latency")) {
        resultDelay = rdLatency;
        break;
      }

  if (!resultDelay) {
    return op->emitError("Could not find a read port of the memref.");
  }
  Builder builder(op);
  op->setAttr("result_delays", builder.getArrayAttr(resultDelay));
  return success();
}

LogicalResult HIRPragma::visitOp(mlir::func::CallOp op) {
  auto *calleeOp = getOperation().lookupSymbol(op.getCalleeAttr());
  SmallVector<Attribute> resultDelays;
  for (auto resAttr : calleeOp->getAttrOfType<ArrayAttr>("res_attrs"))
    resultDelays.push_back(
        resAttr.dyn_cast<DictionaryAttr>().getAs<IntegerAttr>("hir.delay"));

  Builder builder(op);
  op->setAttr("result_delays", builder.getArrayAttr(resultDelays));
  return success();
}

LogicalResult HIRPragma::visitOp(mlir::arith::NegFOp op) {
  OpBuilder builder(op);
  builder.setInsertionPoint(op->getParentOfType<mlir::func::FuncOp>());
  auto funcOp = builder.create<mlir::func::FuncOp>(
      builder.getUnknownLoc(), "neg_f32",
      FunctionType::get(builder.getContext(), {op.getResult().getType()},
                        op.getResult().getType()));
  funcOp.setPrivate();
  funcOp->setAttr("hwAccel", builder.getUnitAttr());

  auto zeroDelayAttr = builder.getDictionaryAttr(
      builder.getNamedAttr("hir.delay", builder.getI64IntegerAttr(0)));
  funcOp->setAttr("arg_attrs", builder.getArrayAttr(zeroDelayAttr));
  funcOp->setAttr("res_attrs", builder.getArrayAttr(zeroDelayAttr));
  funcOp->setAttr("argNames", builder.getStrArrayAttr({"a"}));
  funcOp->setAttr("resultNames", builder.getStrArrayAttr({"out"}));

  builder.setInsertionPoint(op);
  auto newOp = builder.create<mlir::func::CallOp>(op->getLoc(), funcOp,
                                                  op->getOperands());
  newOp->setAttr("result_delays", builder.getI64ArrayAttr({0}));
  op->replaceAllUsesWith(newOp);
  op.erase();
  return success();
}

LogicalResult HIRPragma::visitOp(mlir::LLVM::UndefOp op) {
  for (auto *user : op.getResult().getUsers()) {
    if (!isa<mlir::AffineStoreOp>(user))
      return user->emitError(
          "Only affine.store op uses for llvm.mlir.undef is allowed.");
    user->erase();
  }
  op->erase();
  return success();
}
//-----------------------------------------------------------------------------
std::unique_ptr<mlir::Pass> circt::createHIRPragmaPass() {
  return std::make_unique<HIRPragma>();
}