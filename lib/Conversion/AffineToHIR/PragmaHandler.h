#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "llvm/ADT/Optional.h"
struct MemrefPragmaHandler {
  enum PortKind { READ_ONLY, WRITE_ONLY, READ_WRITE };
  enum RAMKind {
    SMP, // SIMPLE MULTI-PORT RAM
    TMP  // TRUE MULTI-PORT RAM
  };
  MemrefPragmaHandler(mlir::Value memref);
  PortKind getPortKind(int portNum);
  int64_t getRdLatency();
  int64_t getWrLatency();
  /// Get the rd port location in the set of ports.
  int64_t getRdPortID(int64_t n);
  int64_t getWrPortID(int64_t n);
  size_t getNumRdPorts();
  size_t getNumWrPorts();
  RAMKind getRAMKind();

private:
  llvm::SmallVector<PortKind, 2> ports;
  llvm::Optional<int64_t> rdLatency;
  llvm::Optional<int64_t> wrLatency;
  llvm::SmallVector<int64_t, 2> rdPorts;
  llvm::SmallVector<int64_t, 2> wrPorts;
  RAMKind ramKind;
};

struct AffineForPragmaHandler {
  int64_t getII();
  AffineForPragmaHandler(mlir::AffineForOp op);

private:
  int64_t ii;
};