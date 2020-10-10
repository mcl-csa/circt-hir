#ifndef __HIRVERILOGVALUE__
#define __HIRVERILOGVALUE__

#include "Helpers.h"
#include "circt/Dialect/HIR/HIR.h"
#include "circt/Target/HIRToVerilog/HIRToVerilog.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include <string>

using namespace mlir;
using namespace hir;
using namespace std;

static bool isIntegerType(Type type) {
  if (type.isa<IntegerType>())
    return true;
  else if (auto wireType = type.dyn_cast<WireType>())
    return isIntegerType(wireType.getElementType());
  else if (auto constType = type.dyn_cast<hir::ConstType>())
    return isIntegerType(constType.getElementType());
  return false;
}
class VerilogValue {
public:
  VerilogValue() : type(Type()), name("uninitialized") {}
  VerilogValue(Type type, string name) : type(type), name(name) {}

public:
  int numReads() { return usageInfo.numReads; }
  int numWrites() { return usageInfo.numWrites; }
  int numAccess() { return usageInfo.numReads + usageInfo.numWrites; }
  string strMemrefArgDecl();
  Type getType() { return type; }

private:
  SmallVector<unsigned, 4> getMemrefPackedDims() {
    SmallVector<unsigned, 4> out;
    auto memrefType = type.dyn_cast<hir::MemrefType>();
    auto shape = memrefType.getShape();
    auto packing = memrefType.getPacking();
    for (int i = 0; i < shape.size(); i++) {
      bool dimIsPacked = false;
      for (auto p : packing) {
        if (i == shape.size() - 1 - p)
          dimIsPacked = true;
      }
      if (dimIsPacked) {
        auto dim = shape[i];
        out.push_back(dim);
      }
    }
    return out;
  }

  string strMemrefDistDims() {
    string out;
    SmallVector<unsigned, 4> distDims = getMemrefDistDims();
    for (auto dim : distDims) {
      out += "[" + to_string(dim - 1) + ":0]";
    }
    return out;
  }
  SmallVector<unsigned, 4> getMemrefDistDims() {
    SmallVector<unsigned, 4> out;
    auto memrefType = type.dyn_cast<hir::MemrefType>();
    auto shape = memrefType.getShape();
    auto packing = memrefType.getPacking();
    for (int i = 0; i < shape.size(); i++) {
      bool dimIsPacked = false;
      for (auto p : packing) {
        if (i == shape.size() - 1 - p)
          dimIsPacked = true;
      }
      if (!dimIsPacked)
        out.push_back(shape[i]);
    }
    return out;
  }

  string strWireInput() { return name + "_input"; }
  string strMemrefAddrValid() const { return name + "_addr_valid"; }
  string strMemrefAddrInput() { return name + "_addr_input"; }
  string strMemrefWrDataValid() { return name + "_wr_data_valid"; }
  string strMemrefWrDataInput() { return name + "_wr_data_input"; }
  string strMemrefRdEnInput() { return name + "_rd_en_input"; }
  string strMemrefWrEnInput() { return name + "_wr_en_input"; }

  string buildEnableSelectorStr(string str_en, string str_en_input,
                                unsigned numInputs) {
    string out;
    auto distDims = getMemrefDistDims();
    string distDimsStr = strMemrefDistDims();
    string distDimAccessStr = "";

    // Define en_input signal.
    out += "wire [$numInputsMinus1:0] $str_en_input $distDimsStr;\n";

    // Print generate loops for distributed dimensions.
    if (!distDims.empty())
      out += "generate\n";
    for (int i = 0; i < distDims.size(); i++) {
      string str_i = "i" + to_string(i);
      string str_dim = to_string(distDims[i]);
      out += "for(genvar " + str_i + " = 0; " + str_i + " < " + str_dim + ";" +
             str_i + "=" + str_i + " + 1) begin\n";
      distDimAccessStr += "[" + str_i + "]";
    }

    // Assign the enable signal using en_input.
    out += "assign $str_en $distDimAccessStr =| $str_en_input "
           "$distDimAccessStr;\n";

    // Print end/endgenerate.
    for (int i = 0; i < distDims.size(); i++) {
      out += "end\n";
    }
    if (!distDims.empty())
      out += "endgenerate\n";

    findAndReplaceAll(out, "$numInputsMinus1", to_string(numInputs - 1));
    findAndReplaceAll(out, "$str_en_input", str_en_input);
    findAndReplaceAll(out, "$str_en", str_en);
    findAndReplaceAll(out, "$distDimAccessStr", distDimAccessStr);
    findAndReplaceAll(out, "$distDimsStr", distDimsStr);
    return out;
  }

  string buildDataSelectorStr(string str_v, string str_v_valid,
                              string str_v_input, unsigned numInputs,
                              unsigned dataWidth) {
    string out;
    auto distDims = getMemrefDistDims();
    string distDimsStr = strMemrefDistDims();
    string distDimAccessStr = "";

    // Define the valid and input wire arrays.
    out = "wire $v_valid $distDimsStr [$numInputsMinus1:0];\n"
          "wire [$dataWidthMinus1:0] $v_input $distDimsStr "
          "[$numInputsMinus1:0];\n ";

    // Print generate loops for distributed dimensions.
    if (!distDims.empty())
      out += "generate\n";
    for (int i = 0; i < distDims.size(); i++) {
      string str_i = "i" + to_string(i);
      string str_dim = to_string(distDims[i]);
      out += "for(genvar " + str_i + " = 0; " + str_i + " < " + str_dim + ";" +
             str_i + "=" + str_i + " + 1) begin\n";
      distDimAccessStr += "[" + str_i + "]";
    }

    // Assign the data bus($v) using valid and input arrays.
    out += "always@(*) begin\n"
           "if($v_valid_access[0] )\n$v = "
           "$v_input_access[0];\n";
    for (int i = 1; i < numInputs; i++) {
      out += "else if ($v_valid_access[" + to_string(i) + "])\n";
      out += "$v = $v_input_access[" + to_string(i) + "];\n";
    }
    out += "else\n $v = $dataWidth'd0;\n";
    out += "end\n";

    // Print end/endgenerate.
    for (int i = 0; i < distDims.size(); i++) {
      out += "end\n";
    }
    if (!distDims.empty())
      out += "endgenerate\n";

    findAndReplaceAll(out, "$v_valid_access", str_v_valid + distDimAccessStr);
    findAndReplaceAll(out, "$v_input_access", str_v_input + distDimAccessStr);
    findAndReplaceAll(out, "$v_valid", str_v_valid);
    findAndReplaceAll(out, "$v_input", str_v_input);
    findAndReplaceAll(out, "$v", str_v + distDimAccessStr);
    findAndReplaceAll(out, "$dataWidthMinus1", to_string(dataWidth - 1));
    findAndReplaceAll(out, "$dataWidth", to_string(dataWidth));
    findAndReplaceAll(out, "$numInputsMinus1", to_string(numInputs - 1));
    findAndReplaceAll(out, "$distDimsStr", distDimsStr);
    return out;
  }

  string strMemrefAddrValid(unsigned idx) {
    return strMemrefAddrValid() + "[" + to_string(idx) + "]";
  }
  string strMemrefAddrInput(unsigned idx) {
    return strMemrefAddrInput() + "[" + to_string(idx) + "]";
  }
  string strMemrefDistDimAccess(ArrayRef<VerilogValue *> addr) const {
    string out;
    auto packing = type.dyn_cast<hir::MemrefType>().getPacking();
    for (int i = 0; i < addr.size(); i++) {
      bool isDistDim = true;
      for (auto p : packing) {
        if (p == addr.size() - 1 - i)
          isDistDim = false;
      }
      if (isDistDim) {
        VerilogValue *v = addr[i];
        if (v->isIntegerConst())
          out += "[" + to_string(v->getIntegerConst()) + "]";
        else
          out += "[" + v->strWire() + " /*ERROR: expected constant.*/]";
      }
    }
    return out;
  }

public:
  void setIntegerConst(int value) {
    isConstValue = true;
    constValue.val_int = value;
  }

  bool isIntegerConst() const { return isConstValue && isIntegerType(); }
  bool getIntegerConst() const {
    assert(isIntegerConst());
    return constValue.val_int;
  }
  string strMemrefSel() {
    // if (numAccess() == 0)
    //  return "//Unused memref " + strWire() + ".\n";
    stringstream output;
    auto str_addr = strMemrefAddr();
    // print addr bus selector.
    unsigned addrWidth = calcAddrWidth(type.dyn_cast<MemrefType>());
    if (addrWidth > 0) {
      output << buildDataSelectorStr(strMemrefAddr(), strMemrefAddrValid(),
                                     strMemrefAddrInput(), numAccess(),
                                     addrWidth);
      output << "\n";
    }
    // print rd_en selector.
    if (numReads() > 0) {
      output << buildEnableSelectorStr(strMemrefRdEn(), strMemrefRdEnInput(),
                                       numReads());
      output << "\n";
    }

    // print write bus selector.
    if (numWrites() > 0) {
      unsigned dataWidth =
          ::getBitWidth(type.dyn_cast<MemrefType>().getElementType());
      output << buildEnableSelectorStr(strMemrefWrEn(), strMemrefWrEnInput(),
                                       numWrites());
      output << buildDataSelectorStr(strMemrefWrData(), strMemrefWrDataValid(),
                                     strMemrefWrDataInput(), numWrites(),
                                     dataWidth);
      output << "\n";
    }
    return output.str();
  }

  string strWire() const {
    if (name == "") {
      return "/*ERROR: Anonymus variable.*/";
    }
    return name;
  }

  unsigned getBitWidth() const { return ::getBitWidth(type); }
  bool isIntegerType() const { return ::isIntegerType(type); }
  /// Checks if the type is implemented as a verilog wire or an array of wires.
  bool isSimpleType() const {
    if (isIntegerType())
      return true;
    else if (type.isa<hir::TimeType>())
      return true;
    return false;
  }
  string strWireDecl() {
    assert(name != "");
    assert(isSimpleType());
    if (auto wireType = type.dyn_cast<hir::WireType>()) {
      auto shape = wireType.getShape();
      auto elementType = wireType.getElementType();
      auto elementWidthStr = to_string(::getBitWidth(elementType) - 1) + ":0";
      string distDimsStr = "";
      for (auto dim : shape) {
        distDimsStr += "[" + to_string(dim - 1) + ":0]";
      }
      return "[" + elementWidthStr + "] " + strWire() + distDimsStr;
    } else {
      string out;
      if (getBitWidth() > 1)
        out += "[" + to_string(getBitWidth() - 1) + ":0] ";
      return out + strWire();
    }
  }
  string strConstOrError(int n = 0) const {
    if (isIntegerConst()) {
      string constStr = to_string(getIntegerConst() + n);
      if (name == "")
        return constStr;
      return "/*" + name + "=*/ " + constStr;
    }
    fprintf(stderr, "/*ERROR: Expected constant. Found %s + %d */",
            strWire().c_str(), n);
    assert(false);
  }

  string strConstOrWire() const {
    if (isIntegerConst()) {
      string vlogDecimalLiteral =
          to_string(::getBitWidth(type)) + "'d" + to_string(getIntegerConst());
      if (name == "")
        return vlogDecimalLiteral;
      return "/*" + name + "=*/ " + vlogDecimalLiteral;
    }
    return strWire();
  }

  string strWireValid() { return name + "_valid"; }
  string strWireInput(unsigned idx) {
    return strWireInput() + "[" + to_string(idx) + "]";
  }
  string strDelayedWire() const { return name + "delay"; }
  string strDelayedWire(unsigned delay) {
    updateMaxDelay(delay);
    return strDelayedWire() + "[" + to_string(delay) + "]";
  }

  string strMemrefAddr() { return name + "_addr"; }
  string strMemrefAddrValid(ArrayRef<VerilogValue *> addr, unsigned idx) const {
    string distDimAccessStr = strMemrefDistDimAccess(addr);
    return strMemrefAddrValid() + distDimAccessStr + "[" + to_string(idx) + "]";
  }
  string strMemrefAddrInput(ArrayRef<VerilogValue *> addr, unsigned idx) {
    string distDimAccessStr = strMemrefDistDimAccess(addr);
    return strMemrefAddrInput() + distDimAccessStr + "[" + to_string(idx) + "]";
  }

  string strMemrefRdData() { return name + "_rd_data"; }
  string strMemrefRdData(ArrayRef<VerilogValue *> addr) {
    string distDimAccessStr = strMemrefDistDimAccess(addr);
    return strMemrefRdData() + distDimAccessStr;
  }

  string strMemrefWrData() { return name + "_wr_data"; }
  string strMemrefWrDataValid(ArrayRef<VerilogValue *> addr, unsigned idx) {
    string distDimAccessStr = strMemrefDistDimAccess(addr);
    return strMemrefWrDataValid() + distDimAccessStr + "[" + to_string(idx) +
           "]";
  }
  string strMemrefWrDataInput(ArrayRef<VerilogValue *> addr, unsigned idx) {
    string distDimAccessStr = strMemrefDistDimAccess(addr);
    return strMemrefWrDataInput() + distDimAccessStr + "[" + to_string(idx) +
           "]";
  }

  string strMemrefRdEn() { return name + "_rd_en"; }
  string strMemrefRdEnInput(ArrayRef<VerilogValue *> addr, unsigned idx) {
    string distDimAccessStr = strMemrefDistDimAccess(addr);
    return strMemrefRdEnInput() + distDimAccessStr + "[" + to_string(idx) + "]";
  }

  string strMemrefWrEn() { return name + "_wr_en"; }
  string strMemrefWrEnInput(ArrayRef<VerilogValue *> addr, unsigned idx) {
    string distDimAccessStr = strMemrefDistDimAccess(addr);
    return strMemrefWrEnInput() + distDimAccessStr + "[" + to_string(idx) + "]";
  }
  void incNumReads() { usageInfo.numReads++; }
  void incNumWrites() { usageInfo.numWrites++; }

  int getMaxDelay() const { return maxDelay; }

private:
  void updateMaxDelay(int delay) {
    maxDelay = maxDelay > delay ? maxDelay : delay;
  }
  bool isConstValue = false;
  union {
    int val_int;
  } constValue;
  struct {
    unsigned numReads = 0;
    unsigned numWrites = 0;
  } usageInfo;
  Type type;
  string name;
  int maxDelay = 0;
};

string VerilogValue::strMemrefArgDecl() {
  string out;
  MemrefType memrefTy = type.dyn_cast<MemrefType>();
  hir::Details::PortKind port = memrefTy.getPort();
  string portString =
      ((port == hir::Details::r) ? "r"
                                 : (port == hir::Details::w) ? "w" : "rw");
  out += "//MemrefType : port = " + portString + ".\n";
  unsigned addrWidth = calcAddrWidth(memrefTy);
  string distDimsStr = strMemrefDistDims();
  bool printComma = false;
  unsigned dataWidth = ::getBitWidth(memrefTy.getElementType());
  if (addrWidth > 0) { // add dims may be distributed.
    out += "output reg[" + to_string(addrWidth - 1) + ":0] " + strMemrefAddr() +
           distDimsStr;
    printComma = true;
  }
  if (port == hir::Details::r || port == hir::Details::rw) {
    if (printComma)
      out += ",\n";
    out += "output wire " + strMemrefRdEn() + distDimsStr;
    out += ",\ninput wire[" + to_string(dataWidth - 1) + ":0] " +
           strMemrefRdData(SmallVector<VerilogValue *, 4>()) + distDimsStr;
    printComma = true;
  }
  if (port == hir::Details::w || port == hir::Details::rw) {
    if (printComma)
      out += ",\n";
    out += "output wire " + strMemrefWrEn() + distDimsStr;
    out += ",\noutput reg[" + to_string(dataWidth - 1) + ":0] " +
           strMemrefWrData() + distDimsStr;
  }
  return out;
}
#endif
