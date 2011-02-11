//===-- OR1KISelDAGToDAG.cpp - A dag to dag inst selector for OR1K ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the OR1K target.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "mblaze-isel"
#include "OR1K.h"
#include "OR1KMachineFunctionInfo.h"
#include "OR1KRegisterInfo.h"
#include "OR1KSubtarget.h"
#include "OR1KTargetMachine.h"
#include "llvm/GlobalValue.h"
#include "llvm/Instructions.h"
#include "llvm/Intrinsics.h"
#include "llvm/Support/CFG.h"
#include "llvm/Type.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
// Instruction Selector Implementation
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// OR1KDAGToDAGISel - OR1K specific code to select OR1K machine
// instructions for SelectionDAG operations.
//===----------------------------------------------------------------------===//
namespace {

class OR1KDAGToDAGISel : public SelectionDAGISel {

  /// TM - Keep a reference to OR1KTargetMachine.
  OR1KTargetMachine &TM;

  /// Subtarget - Keep a pointer to the OR1KSubtarget around so that we can
  /// make the right decision when generating code for different targets.
  const OR1KSubtarget &Subtarget;

public:
  explicit OR1KDAGToDAGISel(OR1KTargetMachine &tm) :
  SelectionDAGISel(tm),
  TM(tm), Subtarget(tm.getSubtarget<OR1KSubtarget>()) {}

  // Pass Name
  virtual const char *getPassName() const {
    return "OR1K DAG->DAG Pattern Instruction Selection";
  }
private:
  // Include the pieces autogenerated from the target description.
  #include "OR1KGenDAGISel.inc"

  /// getTargetMachine - Return a reference to the TargetMachine, casted
  /// to the target-specific type.
  const OR1KTargetMachine &getTargetMachine() {
    return static_cast<const OR1KTargetMachine &>(TM);
  }

  /// getInstrInfo - Return a reference to the TargetInstrInfo, casted
  /// to the target-specific type.
  const OR1KInstrInfo *getInstrInfo() {
    return getTargetMachine().getInstrInfo();
  }

  SDNode *Select(SDNode *N);

  // Complex Pattern.
  bool SelectAddr(SDNode *Op, SDValue N,
                  SDValue &Base, SDValue &Offset);

  // Address Selection
  bool SelectAddrRegReg(SDNode *Op, SDValue N, SDValue &Base, SDValue &Index);
  bool SelectAddrRegImm(SDNode *Op, SDValue N, SDValue &Disp, SDValue &Base);

  // getI32Imm - Return a target constant with the specified value, of type i32.
  inline SDValue getI32Imm(unsigned Imm) {
    return CurDAG->getTargetConstant(Imm, MVT::i32);
  }
};

}

/// isIntS32Immediate - This method tests to see if the node is either a 32-bit
/// or 64-bit immediate, and if the value can be accurately represented as a
/// sign extension from a 32-bit value.  If so, this returns true and the
/// immediate.
static bool isIntS32Immediate(SDNode *N, int32_t &Imm) {
  unsigned Opc = N->getOpcode();
  if (Opc != ISD::Constant)
    return false;

  Imm = (int32_t)cast<ConstantSDNode>(N)->getZExtValue();
  if (N->getValueType(0) == MVT::i32)
    return Imm == (int32_t)cast<ConstantSDNode>(N)->getZExtValue();
  else
    return Imm == (int64_t)cast<ConstantSDNode>(N)->getZExtValue();
}

static bool isIntS32Immediate(SDValue Op, int32_t &Imm) {
  return isIntS32Immediate(Op.getNode(), Imm);
}


/// SelectAddressRegReg - Given the specified addressed, check to see if it
/// can be represented as an indexed [r+r] operation.  Returns false if it
/// can be more efficiently represented with [r+imm].
bool OR1KDAGToDAGISel::
SelectAddrRegReg(SDNode *Op, SDValue N, SDValue &Base, SDValue &Index) {
  if (N.getOpcode() == ISD::FrameIndex) return false;
  if (N.getOpcode() == ISD::TargetExternalSymbol ||
      N.getOpcode() == ISD::TargetGlobalAddress)
    return false;  // direct calls.

  int32_t imm = 0;
  if (N.getOpcode() == ISD::ADD || N.getOpcode() == ISD::OR) {
    if (isIntS32Immediate(N.getOperand(1), imm))
      return false;    // r+i

    if (N.getOperand(0).getOpcode() == ISD::TargetJumpTable ||
        N.getOperand(1).getOpcode() == ISD::TargetJumpTable)
      return false; // jump tables.

    Base = N.getOperand(1);
    Index = N.getOperand(0);
    return true;
  }

  return false;
}

/// Returns true if the address N can be represented by a base register plus
/// a signed 32-bit displacement [r+imm], and if it is not better
/// represented as reg+reg.
bool OR1KDAGToDAGISel::
SelectAddrRegImm(SDNode *Op, SDValue N, SDValue &Disp, SDValue &Base) {
  // If this can be more profitably realized as r+r, fail.
  if (SelectAddrRegReg(Op, N, Disp, Base))
    return false;

  if (N.getOpcode() == ISD::ADD || N.getOpcode() == ISD::OR) {
    int32_t imm = 0;
    if (isIntS32Immediate(N.getOperand(1), imm)) {
      Disp = CurDAG->getTargetConstant(imm, MVT::i32);
      if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N.getOperand(0))) {
        Base = CurDAG->getTargetFrameIndex(FI->getIndex(), N.getValueType());
      } else {
        Base = N.getOperand(0);
      }
      DEBUG( errs() << "WESLEY: Using Operand Immediate\n" );
      return true; // [r+i]
    }
  } else if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N)) {
    // Loading from a constant address.
    uint32_t Imm = CN->getZExtValue();
    Disp = CurDAG->getTargetConstant(Imm, CN->getValueType(0));
    Base = CurDAG->getRegister(OR1K::R0, CN->getValueType(0));
    DEBUG( errs() << "WESLEY: Using Constant Node\n" );
    return true;
  }

  Disp = CurDAG->getTargetConstant(0, TM.getTargetLowering()->getPointerTy());
  if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N))
    Base = CurDAG->getTargetFrameIndex(FI->getIndex(), N.getValueType());
  else
    Base = N;
  return true;      // [r+0]
}

/// ComplexPattern used on OR1KInstrInfo
/// Used on OR1K Load/Store instructions
bool OR1KDAGToDAGISel::
SelectAddr(SDNode *Op, SDValue Addr, SDValue &Offset, SDValue &Base) {
  // if Address is FI, get the TargetFrameIndex.
  if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
    Base   = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i32);
    Offset = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }

  // on PIC code Load GA
  if (TM.getRelocationModel() == Reloc::PIC_) {
    if ((Addr.getOpcode() == ISD::TargetGlobalAddress) ||
        (Addr.getOpcode() == ISD::TargetConstantPool) ||
        (Addr.getOpcode() == ISD::TargetJumpTable)){
      Base   = CurDAG->getRegister(OR1K::R15, MVT::i32);
      Offset = Addr;
      return true;
    }
  } else {
    if ((Addr.getOpcode() == ISD::TargetExternalSymbol ||
        Addr.getOpcode() == ISD::TargetGlobalAddress))
      return false;
  }

  // Operand is a result from an ADD.
  if (Addr.getOpcode() == ISD::ADD) {
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1))) {
      if (isUInt<16>(CN->getZExtValue())) {

        // If the first operand is a FI, get the TargetFI Node
        if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>
                                    (Addr.getOperand(0))) {
          Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i32);
        } else {
          Base = Addr.getOperand(0);
        }

        Offset = CurDAG->getTargetConstant(CN->getZExtValue(), MVT::i32);
        return true;
      }
    }
  }

  Base   = Addr;
  Offset = CurDAG->getTargetConstant(0, MVT::i32);
  return true;
}

/// Select instructions not customized! Used for
/// expanded, promoted and normal instructions
SDNode* OR1KDAGToDAGISel::Select(SDNode *Node) {
  unsigned Opcode = Node->getOpcode();
  DebugLoc dl = Node->getDebugLoc();

  // Dump information about the Node being selected
  DEBUG(errs() << "Selecting: "; Node->dump(CurDAG); errs() << "\n");

  // If we have a custom node, we already have selected!
  if (Node->isMachineOpcode()) {
    DEBUG(errs() << "== "; Node->dump(CurDAG); errs() << "\n");
    return NULL;
  }

  ///
  // Instruction Selection not handled by the auto-generated
  // tablegen selection should be handled here.
  ///
  switch(Opcode) {
    default: break;

    case ISD::FrameIndex: {
        SDValue imm = CurDAG->getTargetConstant(0, MVT::i32);
        int FI = dyn_cast<FrameIndexSDNode>(Node)->getIndex();
        EVT VT = Node->getValueType(0);
        SDValue TFI = CurDAG->getTargetFrameIndex(FI, VT);
        unsigned Opc = OR1K::ADDI;
        if (Node->hasOneUse())
          return CurDAG->SelectNodeTo(Node, Opc, VT, TFI, imm);
        return CurDAG->getMachineNode(Opc, dl, VT, TFI, imm);
    }
  }

  // Select the default instruction
  SDNode *ResNode = SelectCode(Node);

  DEBUG(errs() << "=> ");
  if (ResNode == NULL || ResNode == Node)
    DEBUG(Node->dump(CurDAG));
  else
    DEBUG(ResNode->dump(CurDAG));
  DEBUG(errs() << "\n");
  return ResNode;
}

/// createOR1KISelDag - This pass converts a legalized DAG into a
/// OR1K-specific DAG, ready for instruction scheduling.
FunctionPass *llvm::createOR1KISelDag(OR1KTargetMachine &TM) {
  return new OR1KDAGToDAGISel(TM);
}