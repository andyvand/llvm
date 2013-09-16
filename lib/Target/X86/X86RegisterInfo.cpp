//===-- X86RegisterInfo.cpp - X86 Register Information --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the X86 implementation of the TargetRegisterInfo class.
// This file is responsible for the frame pointer elimination optimization
// on X86.
//
//===----------------------------------------------------------------------===//

#include "X86RegisterInfo.h"
#include "X86.h"
#include "X86InstrBuilder.h"
#include "X86MachineFunctionInfo.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetFrameLowering.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#define GET_REGINFO_TARGET_DESC
#include "X86GenRegisterInfo.inc"

using namespace llvm;

cl::opt<bool>
ForceStackAlign("force-align-stack",
                 cl::desc("Force align the stack to the minimum alignment"
                           " needed for the function."),
                 cl::init(false), cl::Hidden);

static cl::opt<bool>
EnableBasePointer("x86-use-base-pointer", cl::Hidden, cl::init(true),
          cl::desc("Enable use of a base pointer for complex stack frames"));

X86RegisterInfo::X86RegisterInfo(X86TargetMachine &tm)
  : X86GenRegisterInfo((tm.getSubtarget<X86Subtarget>().is64Bit()
                         ? X86::RIP : X86::EIP),
                       X86_MC::getDwarfRegFlavour(tm.getTargetTriple(), false),
                       X86_MC::getDwarfRegFlavour(tm.getTargetTriple(), true),
                       (tm.getSubtarget<X86Subtarget>().is64Bit()
                         ? X86::RIP : X86::EIP)),
                       TM(tm) {
  X86_MC::InitLLVM2SEHRegisterMapping(this);

  // Cache some information.
  const X86Subtarget *Subtarget = &TM.getSubtarget<X86Subtarget>();
  Is64Bit = Subtarget->is64Bit();
  IsWin64 = Subtarget->isTargetWin64();

  if (Is64Bit) {
    SlotSize = 8;
    StackPtr = X86::RSP;
    FramePtr = X86::RBP;
  } else {
    SlotSize = 4;
    StackPtr = X86::ESP;
    FramePtr = X86::EBP;
  }
  // Use a callee-saved register as the base pointer.  These registers must
  // not conflict with any ABI requirements.  For example, in 32-bit mode PIC
  // requires GOT in the EBX register before function calls via PLT GOT pointer.
  BasePtr = Is64Bit ? X86::RBX : X86::ESI;
}

/// getCompactUnwindRegNum - This function maps the register to the number for
/// compact unwind encoding. Return -1 if the register isn't valid.
int X86RegisterInfo::getCompactUnwindRegNum(unsigned RegNum, bool isEH) const {
  switch (getLLVMRegNum(RegNum, isEH)) {
  case X86::EBX: case X86::RBX: return 1;
  case X86::ECX: case X86::R12: return 2;
  case X86::EDX: case X86::R13: return 3;
  case X86::EDI: case X86::R14: return 4;
  case X86::ESI: case X86::R15: return 5;
  case X86::EBP: case X86::RBP: return 6;
  }

  return -1;
}

bool
X86RegisterInfo::trackLivenessAfterRegAlloc(const MachineFunction &MF) const {
  // Only enable when post-RA scheduling is enabled and this is needed.
  return TM.getSubtargetImpl()->postRAScheduler();
}

int
X86RegisterInfo::getSEHRegNum(unsigned i) const {
  return getEncodingValue(i);
}

const TargetRegisterClass *
X86RegisterInfo::getSubClassWithSubReg(const TargetRegisterClass *RC,
                                       unsigned Idx) const {
  // The sub_8bit sub-register index is more constrained in 32-bit mode.
  // It behaves just like the sub_8bit_hi index.
  if (!Is64Bit && Idx == X86::sub_8bit)
    Idx = X86::sub_8bit_hi;

  // Forward to TableGen's default version.
  return X86GenRegisterInfo::getSubClassWithSubReg(RC, Idx);
}

const TargetRegisterClass *
X86RegisterInfo::getMatchingSuperRegClass(const TargetRegisterClass *A,
                                          const TargetRegisterClass *B,
                                          unsigned SubIdx) const {
  // The sub_8bit sub-register index is more constrained in 32-bit mode.
  if (!Is64Bit && SubIdx == X86::sub_8bit) {
    A = X86GenRegisterInfo::getSubClassWithSubReg(A, X86::sub_8bit_hi);
    if (!A)
      return 0;
  }
  return X86GenRegisterInfo::getMatchingSuperRegClass(A, B, SubIdx);
}

const TargetRegisterClass*
X86RegisterInfo::getLargestLegalSuperClass(const TargetRegisterClass *RC) const{
  // Don't allow super-classes of GR8_NOREX.  This class is only used after
  // extrating sub_8bit_hi sub-registers.  The H sub-registers cannot be copied
  // to the full GR8 register class in 64-bit mode, so we cannot allow the
  // reigster class inflation.
  //
  // The GR8_NOREX class is always used in a way that won't be constrained to a
  // sub-class, so sub-classes like GR8_ABCD_L are allowed to expand to the
  // full GR8 class.
  if (RC == &X86::GR8_NOREXRegClass)
    return RC;

  const TargetRegisterClass *Super = RC;
  TargetRegisterClass::sc_iterator I = RC->getSuperClasses();
  do {
    switch (Super->getID()) {
    case X86::GR8RegClassID:
    case X86::GR16RegClassID:
    case X86::GR32RegClassID:
    case X86::GR64RegClassID:
    case X86::FR32RegClassID:
    case X86::FR64RegClassID:
    case X86::RFP32RegClassID:
    case X86::RFP64RegClassID:
    case X86::RFP80RegClassID:
    case X86::VR128RegClassID:
    case X86::VR256RegClassID:
      // Don't return a super-class that would shrink the spill size.
      // That can happen with the vector and float classes.
      if (Super->getSize() == RC->getSize())
        return Super;
    }
    Super = *I++;
  } while (Super);
  return RC;
}

const TargetRegisterClass *
X86RegisterInfo::getPointerRegClass(const MachineFunction &MF, unsigned Kind)
                                                                         const {
  const X86Subtarget &Subtarget = TM.getSubtarget<X86Subtarget>();
  switch (Kind) {
  default: llvm_unreachable("Unexpected Kind in getPointerRegClass!");
  case 0: // Normal GPRs.
    if (Subtarget.isTarget64BitLP64())
      return &X86::GR64RegClass;
    return &X86::GR32RegClass;
  case 1: // Normal GPRs except the stack pointer (for encoding reasons).
    if (Subtarget.isTarget64BitLP64())
      return &X86::GR64_NOSPRegClass;
    return &X86::GR32_NOSPRegClass;
  case 2: // Available for tailcall (not callee-saved GPRs).
    if (Subtarget.isTargetWin64())
      return &X86::GR64_TCW64RegClass;
    else if (Subtarget.is64Bit())
      return &X86::GR64_TCRegClass;

    const Function *F = MF.getFunction();
    bool hasHipeCC = (F ? F->getCallingConv() == CallingConv::HiPE : false);
    if (hasHipeCC)
      return &X86::GR32RegClass;
    return &X86::GR32_TCRegClass;
  }
}

const TargetRegisterClass *
X86RegisterInfo::getCrossCopyRegClass(const TargetRegisterClass *RC) const {
  if (RC == &X86::CCRRegClass) {
    if (Is64Bit)
      return &X86::GR64RegClass;
    else
      return &X86::GR32RegClass;
  }
  return RC;
}

unsigned
X86RegisterInfo::getRegPressureLimit(const TargetRegisterClass *RC,
                                     MachineFunction &MF) const {
  const TargetFrameLowering *TFI = MF.getTarget().getFrameLowering();

  unsigned FPDiff = TFI->hasFP(MF) ? 1 : 0;
  switch (RC->getID()) {
  default:
    return 0;
  case X86::GR32RegClassID:
    return 4 - FPDiff;
  case X86::GR64RegClassID:
    return 12 - FPDiff;
  case X86::VR128RegClassID:
    return TM.getSubtarget<X86Subtarget>().is64Bit() ? 10 : 4;
  case X86::VR64RegClassID:
    return 4;
  }
}
/// isMarkedAsNotPreservedRegister - Returns true if Reg is marked as not
/// preserved even if Reg is a callee saved register.
bool X86RegisterInfo::isMarkedAsNotPreservedRegister(const MachineFunction &MF,
                                                     unsigned Reg) const {
  const X86MachineFunctionInfo *FI = MF.getInfo<X86MachineFunctionInfo>();
  if (!FI)
    return false;

  // If Reg or any its aliasing register is used as passing parameters or
  // returning arguments, then Reg is marked as not preserved.
  for (MCRegAliasIterator AI(Reg, this, true); AI.isValid(); ++AI)
    if (FI->isUsedRegister(*AI))
      return true;

  return false;
}

const uint16_t *
X86RegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  switch (MF->getFunction()->getCallingConv()) {
  case CallingConv::GHC:
  case CallingConv::HiPE:
    return CSR_NoRegs_SaveList;

  case CallingConv::Intel_OCL_BI: {
    bool HasAVX = TM.getSubtarget<X86Subtarget>().hasAVX();
    bool HasAVX512 = TM.getSubtarget<X86Subtarget>().hasAVX512();
    if (HasAVX512 && IsWin64)
      return CSR_Win64_Intel_OCL_BI_AVX512_SaveList;
    if (HasAVX512 && Is64Bit)
      return CSR_64_Intel_OCL_BI_AVX512_SaveList;
    if (HasAVX && IsWin64)
      return CSR_Win64_Intel_OCL_BI_AVX_SaveList;
    if (HasAVX && Is64Bit)
      return CSR_64_Intel_OCL_BI_AVX_SaveList;
    if (!HasAVX && !IsWin64 && Is64Bit)
      return CSR_64_Intel_OCL_BI_SaveList;
    break;
  }

  case CallingConv::X86_RegCall:
    assert(!TM.getSubtarget<X86Subtarget>().isTargetWindows() &&
           "Windows target not supported yet");
    return Is64Bit ? CSR_64_RegCall_SaveList : CSR_32_RegCall_SaveList;

  case CallingConv::Cold:
    if (Is64Bit)
      return CSR_MostRegs_64_SaveList;
    break;

  default:
    break;
  }

  bool CallsEHReturn = MF->getMMI().callsEHReturn();
  if (Is64Bit) {
    if (IsWin64)
      return CSR_Win64_SaveList;
    if (CallsEHReturn)
      return CSR_64EHRet_SaveList;
    return CSR_64_SaveList;
  }
  if (CallsEHReturn)
    return CSR_32EHRet_SaveList;
  return CSR_32_SaveList;
}

const uint32_t*
X86RegisterInfo::getCallPreservedMask(CallingConv::ID CC) const {
  bool HasAVX = TM.getSubtarget<X86Subtarget>().hasAVX();
  bool HasAVX512 = TM.getSubtarget<X86Subtarget>().hasAVX512();

  if (CC == CallingConv::Intel_OCL_BI) {
    if (IsWin64 && HasAVX512)
      return CSR_Win64_Intel_OCL_BI_AVX512_RegMask;
    if (Is64Bit && HasAVX512)
      return CSR_64_Intel_OCL_BI_AVX512_RegMask;
    if (IsWin64 && HasAVX)
      return CSR_Win64_Intel_OCL_BI_AVX_RegMask;
    if (Is64Bit && HasAVX)
      return CSR_64_Intel_OCL_BI_AVX_RegMask;
    if (!HasAVX && !IsWin64 && Is64Bit)
      return CSR_64_Intel_OCL_BI_RegMask;
  }
  if (CC == CallingConv::X86_RegCall) {
    assert(!TM.getSubtarget<X86Subtarget>().isTargetWindows() &&
           "Windows target not supported yet");
    return Is64Bit ? CSR_64_RegCall_RegMask: CSR_32_RegCall_RegMask;
  }
  if (CC == CallingConv::GHC || CC == CallingConv::HiPE)
    return CSR_NoRegs_RegMask;
  if (!Is64Bit)
    return CSR_32_RegMask;
  if (CC == CallingConv::Cold)
    return CSR_MostRegs_64_RegMask;
  if (IsWin64)
    return CSR_Win64_RegMask;
  return CSR_64_RegMask;
}

const uint32_t*
X86RegisterInfo::getNoPreservedMask() const {
  return CSR_NoRegs_RegMask;
}

BitVector X86RegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  const TargetFrameLowering *TFI = MF.getTarget().getFrameLowering();

  // Set the stack-pointer register and its aliases as reserved.
  for (MCSubRegIterator I(X86::RSP, this, /*IncludeSelf=*/true); I.isValid();
       ++I)
    Reserved.set(*I);

  // Set the instruction pointer register and its aliases as reserved.
  for (MCSubRegIterator I(X86::RIP, this, /*IncludeSelf=*/true); I.isValid();
       ++I)
    Reserved.set(*I);

  // Set the frame-pointer register and its aliases as reserved if needed.
  if (TFI->hasFP(MF)) {
    for (MCSubRegIterator I(X86::RBP, this, /*IncludeSelf=*/true); I.isValid();
         ++I)
      Reserved.set(*I);
  }

  // Set the base-pointer register and its aliases as reserved if needed.
  if (hasBasePointer(MF)) {
    CallingConv::ID CC = MF.getFunction()->getCallingConv();
    const uint32_t* RegMask = getCallPreservedMask(CC);
    if (MachineOperand::clobbersPhysReg(RegMask, getBaseRegister()))
      report_fatal_error(
        "Stack realignment in presence of dynamic allocas is not supported with"
        "this calling convention.");

    for (MCSubRegIterator I(getBaseRegister(), this, /*IncludeSelf=*/true);
         I.isValid(); ++I)
      Reserved.set(*I);
  }

  // Mark the segment registers as reserved.
  Reserved.set(X86::CS);
  Reserved.set(X86::SS);
  Reserved.set(X86::DS);
  Reserved.set(X86::ES);
  Reserved.set(X86::FS);
  Reserved.set(X86::GS);

  // Mark the floating point stack registers as reserved.
  for (unsigned n = 0; n != 8; ++n)
    Reserved.set(X86::ST0 + n);

  // Reserve the registers that only exist in 64-bit mode.
  if (!Is64Bit) {
    // These 8-bit registers are part of the x86-64 extension even though their
    // super-registers are old 32-bits.
    Reserved.set(X86::SIL);
    Reserved.set(X86::DIL);
    Reserved.set(X86::BPL);
    Reserved.set(X86::SPL);

    for (unsigned n = 0; n != 8; ++n) {
      // R8, R9, ...
      for (MCRegAliasIterator AI(X86::R8 + n, this, true); AI.isValid(); ++AI)
        Reserved.set(*AI);

      // XMM8, XMM9, ...
      for (MCRegAliasIterator AI(X86::XMM8 + n, this, true); AI.isValid(); ++AI)
        Reserved.set(*AI);
    }
  }
  if (!Is64Bit || !TM.getSubtarget<X86Subtarget>().hasAVX512()) {
    for (unsigned n = 16; n != 32; ++n) {
      for (MCRegAliasIterator AI(X86::XMM0 + n, this, true); AI.isValid(); ++AI)
        Reserved.set(*AI);
    }
  }

  return Reserved;
}

//===----------------------------------------------------------------------===//
// Stack Frame Processing methods
//===----------------------------------------------------------------------===//

bool X86RegisterInfo::hasBasePointer(const MachineFunction &MF) const {
   const MachineFrameInfo *MFI = MF.getFrameInfo();

   if (!EnableBasePointer)
     return false;

   // When we need stack realignment and there are dynamic allocas, we can't
   // reference off of the stack pointer, so we reserve a base pointer.
   //
   // This is also true if the function contain MS-style inline assembly.  We
   // do this because if any stack changes occur in the inline assembly, e.g.,
   // "pusha", then any C local variable or C argument references in the
   // inline assembly will be wrong because the SP is not properly tracked.
   if ((needsStackRealignment(MF) && MFI->hasVarSizedObjects()) ||
       MF.hasMSInlineAsm())
     return true;

   return false;
}

bool X86RegisterInfo::canRealignStack(const MachineFunction &MF) const {
  if (MF.getFunction()->hasFnAttribute("no-realign-stack"))
    return false;

  const MachineFrameInfo *MFI = MF.getFrameInfo();
  const MachineRegisterInfo *MRI = &MF.getRegInfo();

  // Stack realignment requires a frame pointer.  If we already started
  // register allocation with frame pointer elimination, it is too late now.
  if (!MRI->canReserveReg(FramePtr))
    return false;

  // If a base pointer is necessary.  Check that it isn't too late to reserve
  // it.
  if (MFI->hasVarSizedObjects())
    return MRI->canReserveReg(BasePtr);
  return true;
}

bool X86RegisterInfo::needsStackRealignment(const MachineFunction &MF) const {
  const MachineFrameInfo *MFI = MF.getFrameInfo();
  const Function *F = MF.getFunction();
  unsigned StackAlign = TM.getFrameLowering()->getStackAlignment();
  bool requiresRealignment =
    ((MFI->getMaxAlignment() > StackAlign) ||
     F->getAttributes().hasAttribute(AttributeSet::FunctionIndex,
                                     Attribute::StackAlignment));

  // If we've requested that we force align the stack do so now.
  if (ForceStackAlign)
    return canRealignStack(MF);

  return requiresRealignment && canRealignStack(MF);
}

bool X86RegisterInfo::hasReservedSpillSlot(const MachineFunction &MF,
                                           unsigned Reg, int &FrameIdx) const {
  const TargetFrameLowering *TFI = MF.getTarget().getFrameLowering();

  if (Reg == FramePtr && TFI->hasFP(MF)) {
    FrameIdx = MF.getFrameInfo()->getObjectIndexBegin();
    return true;
  }
  return false;
}

void
X86RegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                     int SPAdj, unsigned FIOperandNum,
                                     RegScavenger *RS) const {
  assert(SPAdj == 0 && "Unexpected");

  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getParent()->getParent();
  const TargetFrameLowering *TFI = MF.getTarget().getFrameLowering();
  int FrameIndex = MI.getOperand(FIOperandNum).getIndex();
  unsigned BasePtr;

  unsigned Opc = MI.getOpcode();
  bool AfterFPPop = Opc == X86::TAILJMPm64 || Opc == X86::TAILJMPm;
  if (hasBasePointer(MF))
    BasePtr = (FrameIndex < 0 ? FramePtr : getBaseRegister());
  else if (needsStackRealignment(MF))
    BasePtr = (FrameIndex < 0 ? FramePtr : StackPtr);
  else if (AfterFPPop)
    BasePtr = StackPtr;
  else
    BasePtr = (TFI->hasFP(MF) ? FramePtr : StackPtr);

  // This must be part of a four operand memory reference.  Replace the
  // FrameIndex with base register with EBP.  Add an offset to the offset.
  MI.getOperand(FIOperandNum).ChangeToRegister(BasePtr, false);

  // Now add the frame object offset to the offset from EBP.
  int FIOffset;
  if (AfterFPPop) {
    // Tail call jmp happens after FP is popped.
    const MachineFrameInfo *MFI = MF.getFrameInfo();
    FIOffset = MFI->getObjectOffset(FrameIndex) - TFI->getOffsetOfLocalArea();
  } else
    FIOffset = TFI->getFrameIndexOffset(MF, FrameIndex);

  if (MI.getOperand(FIOperandNum+3).isImm()) {
    // Offset is a 32-bit integer.
    int Imm = (int)(MI.getOperand(FIOperandNum + 3).getImm());
    int Offset = FIOffset + Imm;
    assert((!Is64Bit || isInt<32>((long long)FIOffset + Imm)) &&
           "Requesting 64-bit offset in 32-bit immediate!");
    MI.getOperand(FIOperandNum + 3).ChangeToImmediate(Offset);
  } else {
    // Offset is symbolic. This is extremely rare.
    uint64_t Offset = FIOffset +
      (uint64_t)MI.getOperand(FIOperandNum+3).getOffset();
    MI.getOperand(FIOperandNum + 3).setOffset(Offset);
  }
}

unsigned X86RegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  const TargetFrameLowering *TFI = MF.getTarget().getFrameLowering();
  return TFI->hasFP(MF) ? FramePtr : StackPtr;
}

unsigned X86RegisterInfo::getEHExceptionRegister() const {
  llvm_unreachable("What is the exception register");
}

unsigned X86RegisterInfo::getEHHandlerRegister() const {
  llvm_unreachable("What is the exception handler register");
}

namespace llvm {
unsigned getX86SubSuperRegister(unsigned Reg, MVT::SimpleValueType VT,
                                bool High) {
  switch (VT) {
  default: llvm_unreachable("Unexpected VT");
  case MVT::i8:
    if (High) {
      switch (Reg) {
      default: return getX86SubSuperRegister(Reg, MVT::i64);
      case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
        return X86::SI;
      case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
        return X86::DI;
      case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
        return X86::BP;
      case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
        return X86::SP;
      case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
        return X86::AH;
      case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
        return X86::DH;
      case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
        return X86::CH;
      case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
        return X86::BH;
      }
    } else {
      switch (Reg) {
      default: llvm_unreachable("Unexpected register");
      case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
        return X86::AL;
      case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
        return X86::DL;
      case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
        return X86::CL;
      case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
        return X86::BL;
      case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
        return X86::SIL;
      case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
        return X86::DIL;
      case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
        return X86::BPL;
      case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
        return X86::SPL;
      case X86::R8B: case X86::R8W: case X86::R8D: case X86::R8:
        return X86::R8B;
      case X86::R9B: case X86::R9W: case X86::R9D: case X86::R9:
        return X86::R9B;
      case X86::R10B: case X86::R10W: case X86::R10D: case X86::R10:
        return X86::R10B;
      case X86::R11B: case X86::R11W: case X86::R11D: case X86::R11:
        return X86::R11B;
      case X86::R12B: case X86::R12W: case X86::R12D: case X86::R12:
        return X86::R12B;
      case X86::R13B: case X86::R13W: case X86::R13D: case X86::R13:
        return X86::R13B;
      case X86::R14B: case X86::R14W: case X86::R14D: case X86::R14:
        return X86::R14B;
      case X86::R15B: case X86::R15W: case X86::R15D: case X86::R15:
        return X86::R15B;
      }
    }
  case MVT::i16:
    switch (Reg) {
    default: llvm_unreachable("Unexpected register");
    case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
      return X86::AX;
    case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
      return X86::DX;
    case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
      return X86::CX;
    case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
      return X86::BX;
    case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
      return X86::SI;
    case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
      return X86::DI;
    case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
      return X86::BP;
    case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
      return X86::SP;
    case X86::R8B: case X86::R8W: case X86::R8D: case X86::R8:
      return X86::R8W;
    case X86::R9B: case X86::R9W: case X86::R9D: case X86::R9:
      return X86::R9W;
    case X86::R10B: case X86::R10W: case X86::R10D: case X86::R10:
      return X86::R10W;
    case X86::R11B: case X86::R11W: case X86::R11D: case X86::R11:
      return X86::R11W;
    case X86::R12B: case X86::R12W: case X86::R12D: case X86::R12:
      return X86::R12W;
    case X86::R13B: case X86::R13W: case X86::R13D: case X86::R13:
      return X86::R13W;
    case X86::R14B: case X86::R14W: case X86::R14D: case X86::R14:
      return X86::R14W;
    case X86::R15B: case X86::R15W: case X86::R15D: case X86::R15:
      return X86::R15W;
    }
  case MVT::i32:
    switch (Reg) {
    default: llvm_unreachable("Unexpected register");
    case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
      return X86::EAX;
    case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
      return X86::EDX;
    case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
      return X86::ECX;
    case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
      return X86::EBX;
    case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
      return X86::ESI;
    case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
      return X86::EDI;
    case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
      return X86::EBP;
    case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
      return X86::ESP;
    case X86::R8B: case X86::R8W: case X86::R8D: case X86::R8:
      return X86::R8D;
    case X86::R9B: case X86::R9W: case X86::R9D: case X86::R9:
      return X86::R9D;
    case X86::R10B: case X86::R10W: case X86::R10D: case X86::R10:
      return X86::R10D;
    case X86::R11B: case X86::R11W: case X86::R11D: case X86::R11:
      return X86::R11D;
    case X86::R12B: case X86::R12W: case X86::R12D: case X86::R12:
      return X86::R12D;
    case X86::R13B: case X86::R13W: case X86::R13D: case X86::R13:
      return X86::R13D;
    case X86::R14B: case X86::R14W: case X86::R14D: case X86::R14:
      return X86::R14D;
    case X86::R15B: case X86::R15W: case X86::R15D: case X86::R15:
      return X86::R15D;
    }
  case MVT::i64:
    switch (Reg) {
    default: llvm_unreachable("Unexpected register");
    case X86::AH: case X86::AL: case X86::AX: case X86::EAX: case X86::RAX:
      return X86::RAX;
    case X86::DH: case X86::DL: case X86::DX: case X86::EDX: case X86::RDX:
      return X86::RDX;
    case X86::CH: case X86::CL: case X86::CX: case X86::ECX: case X86::RCX:
      return X86::RCX;
    case X86::BH: case X86::BL: case X86::BX: case X86::EBX: case X86::RBX:
      return X86::RBX;
    case X86::SIL: case X86::SI: case X86::ESI: case X86::RSI:
      return X86::RSI;
    case X86::DIL: case X86::DI: case X86::EDI: case X86::RDI:
      return X86::RDI;
    case X86::BPL: case X86::BP: case X86::EBP: case X86::RBP:
      return X86::RBP;
    case X86::SPL: case X86::SP: case X86::ESP: case X86::RSP:
      return X86::RSP;
    case X86::R8B: case X86::R8W: case X86::R8D: case X86::R8:
      return X86::R8;
    case X86::R9B: case X86::R9W: case X86::R9D: case X86::R9:
      return X86::R9;
    case X86::R10B: case X86::R10W: case X86::R10D: case X86::R10:
      return X86::R10;
    case X86::R11B: case X86::R11W: case X86::R11D: case X86::R11:
      return X86::R11;
    case X86::R12B: case X86::R12W: case X86::R12D: case X86::R12:
      return X86::R12;
    case X86::R13B: case X86::R13W: case X86::R13D: case X86::R13:
      return X86::R13;
    case X86::R14B: case X86::R14W: case X86::R14D: case X86::R14:
      return X86::R14;
    case X86::R15B: case X86::R15W: case X86::R15D: case X86::R15:
      return X86::R15;
    }
  }
}

unsigned get512BitSuperRegister(unsigned Reg) {
  if (Reg >= X86::XMM0 && Reg <= X86::XMM31)
    return X86::ZMM0 + (Reg - X86::XMM0);
  if (Reg >= X86::YMM0 && Reg <= X86::YMM31)
    return X86::ZMM0 + (Reg - X86::YMM0);
  if (Reg >= X86::ZMM0 && Reg <= X86::ZMM31)
    return Reg;
  llvm_unreachable("Unexpected SIMD register");
}

}
