// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <array>
#include <limits>
#include <vector>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/x64Emitter.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"

using namespace Gen;

void Jit64::GenerateConstantOverflow(s64 val)
{
  GenerateConstantOverflow(val > std::numeric_limits<s32>::max() ||
                           val < std::numeric_limits<s32>::min());
}

void Jit64::GenerateConstantOverflow(bool overflow)
{
  if (overflow)
  {
    // XER[OV/SO] = 1
    MOV(8, PPCSTATE(xer_so_ov), Imm8(XER_OV_MASK | XER_SO_MASK));
  }
  else
  {
    // XER[OV] = 0
    AND(8, PPCSTATE(xer_so_ov), Imm8(~XER_OV_MASK));
  }
}

// We could do overflow branchlessly, but unlike carry it seems to be quite a bit rarer.
void Jit64::GenerateOverflow()
{
  FixupBranch jno = J_CC(CC_NO);
  // XER[OV/SO] = 1
  MOV(8, PPCSTATE(xer_so_ov), Imm8(XER_OV_MASK | XER_SO_MASK));
  FixupBranch exit = J();
  SetJumpTarget(jno);
  // XER[OV] = 0
  // We need to do this without modifying flags so as not to break stuff that assumes flags
  // aren't clobbered (carry, branch merging): speed doesn't really matter here (this is really
  // rare).
  static const std::array<u8, 4> ovtable = {{0, 0, XER_SO_MASK, XER_SO_MASK}};
  MOVZX(32, 8, RSCRATCH, PPCSTATE(xer_so_ov));
  LEA(64, RSCRATCH2, MConst(ovtable));
  MOV(8, R(RSCRATCH), MRegSum(RSCRATCH, RSCRATCH2));
  MOV(8, PPCSTATE(xer_so_ov), R(RSCRATCH));
  SetJumpTarget(exit);
}

void Jit64::FinalizeCarry(CCFlags cond)
{
  js.carryFlagSet = false;
  js.carryFlagInverted = false;
  if (js.op->wantsCA)
  {
    // Not actually merging instructions, but the effect is equivalent (we can't have
    // breakpoints/etc in between).
    if (CanMergeNextInstructions(1) && js.op[1].wantsCAInFlags)
    {
      if (cond == CC_C || cond == CC_NC)
      {
        js.carryFlagInverted = cond == CC_NC;
      }
      else
      {
        // convert the condition to a carry flag (is there a better way?)
        SETcc(cond, R(RSCRATCH));
        SHR(8, R(RSCRATCH), Imm8(1));
      }
      LockFlags();
      js.carryFlagSet = true;
    }
    else
    {
      JitSetCAIf(cond);
    }
  }
}

// Unconditional version
void Jit64::FinalizeCarry(bool ca)
{
  js.carryFlagSet = false;
  js.carryFlagInverted = false;
  if (js.op->wantsCA)
  {
    if (CanMergeNextInstructions(1) && js.op[1].wantsCAInFlags)
    {
      if (ca)
        STC();
      else
        CLC();
      LockFlags();
      js.carryFlagSet = true;
    }
    else if (ca)
    {
      JitSetCA();
    }
    else
    {
      JitClearCA();
    }
  }
}

void Jit64::FinalizeCarryOverflow(bool oe, bool inv)
{
  if (oe)
  {
    // Make sure not to lose the carry flags (not a big deal, this path is rare).
    PUSHF();
    // XER[OV] = 0
    AND(8, PPCSTATE(xer_so_ov), Imm8(~XER_OV_MASK));
    FixupBranch jno = J_CC(CC_NO);
    // XER[OV/SO] = 1
    MOV(8, PPCSTATE(xer_so_ov), Imm8(XER_SO_MASK | XER_OV_MASK));
    SetJumpTarget(jno);
    POPF();
  }
  // Do carry
  FinalizeCarry(inv ? CC_NC : CC_C);
}

// Be careful; only set needs_test to false if we can be absolutely sure flags don't need
// to be recalculated and haven't been clobbered. Keep in mind not all instructions set
// sufficient flags -- for example, the flags from SHL/SHR are *not* sufficient for LT/GT
// branches, only EQ.
// The flags from any instruction that may set OF (such as ADD/SUB) can not be used for
// LT/GT either.
void Jit64::ComputeRC(preg_t preg, bool needs_test, bool needs_sext)
{
  RCOpArg arg = gpr.Use(preg, RCMode::Read);
  RegCache::Realize(arg);

  if (arg.IsImm())
  {
    MOV(64, PPCSTATE(cr.fields[0]), Imm32(arg.SImm32()));
  }
  else if (needs_sext)
  {
    MOVSX(64, 32, RSCRATCH, arg);
    MOV(64, PPCSTATE(cr.fields[0]), R(RSCRATCH));
  }
  else
  {
    MOV(64, PPCSTATE(cr.fields[0]), arg);
  }

  if (CheckMergedBranch(0))
  {
    if (arg.IsImm())
    {
      s32 offset = arg.SImm32();
      arg.Unlock();
      DoMergedBranchImmediate(offset);
    }
    else
    {
      if (needs_test)
      {
        TEST(32, arg, arg);
        arg.Unlock();
      }
      else
      {
        // If an operand to the cmp/rc op we're merging with the branch isn't used anymore, it'd be
        // better to flush it here so that we don't have to flush it on both sides of the branch.
        // We don't want to do this if a test is needed though, because it would interrupt macro-op
        // fusion.
        arg.Unlock();
        gpr.Flush(~js.op->gprInUse);
      }
      DoMergedBranchCondition();
    }
  }
}

// we can't do this optimization in the emitter because MOVZX and AND have different effects on
// flags.
void Jit64::AndWithMask(X64Reg reg, u32 mask)
{
  if (mask == 0xff)
    MOVZX(32, 8, reg, R(reg));
  else if (mask == 0xffff)
    MOVZX(32, 16, reg, R(reg));
  else
    AND(32, R(reg), Imm32(mask));
}

// Following static functions are used in conjunction with regimmop
static u32 Add(u32 a, u32 b)
{
  return a + b;
}

static u32 Or(u32 a, u32 b)
{
  return a | b;
}

static u32 And(u32 a, u32 b)
{
  return a & b;
}

static u32 Xor(u32 a, u32 b)
{
  return a ^ b;
}

void Jit64::regimmop(int d, int a, bool binary, u32 value, Operation doop,
                     void (XEmitter::*op)(int, const OpArg&, const OpArg&), bool Rc, bool carry)
{
  bool needs_test = doop == Add;
  // Be careful; addic treats r0 as r0, but addi treats r0 as zero.
  if (a || binary || carry)
  {
    carry &= js.op->wantsCA;
    if (gpr.IsImm(a) && !carry)
    {
      gpr.SetImmediate32(d, doop(gpr.Imm32(a), value));
    }
    else
    {
      RCOpArg Ra = gpr.Use(a, RCMode::Read);
      RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
      RegCache::Realize(Ra, Rd);
      if (doop == Add && Ra.IsSimpleReg() && !carry && d != a)
      {
        LEA(32, Rd, MDisp(Ra.GetSimpleReg(), value));
      }
      else
      {
        if (d != a)
          MOV(32, Rd, Ra);
        (this->*op)(32, Rd, Imm32(value));  // m_GPR[d] = m_GPR[_inst.RA] + _inst.SIMM_16;
      }
    }
    if (carry)
      FinalizeCarry(CC_C);
  }
  else if (doop == Add)
  {
    // a == 0, which for these instructions imply value = 0
    gpr.SetImmediate32(d, value);
  }
  else
  {
    ASSERT_MSG(DYNA_REC, 0, "WTF regimmop");
  }
  if (Rc)
    ComputeRC(d, needs_test, doop != And || (value & 0x80000000));
}

void Jit64::reg_imm(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  u32 d = inst.RD, a = inst.RA, s = inst.RS;
  switch (inst.OPCD)
  {
  case 14:  // addi
    // occasionally used as MOV - emulate, with immediate propagation
    if (gpr.IsImm(a) && d != a && a != 0)
    {
      gpr.SetImmediate32(d, gpr.Imm32(a) + (u32)(s32)inst.SIMM_16);
    }
    else if (inst.SIMM_16 == 0 && d != a && a != 0)
    {
      RCOpArg Ra = gpr.Use(a, RCMode::Read);
      RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
      RegCache::Realize(Ra, Rd);
      MOV(32, Rd, Ra);
    }
    else
    {
      regimmop(d, a, false, (u32)(s32)inst.SIMM_16, Add, &XEmitter::ADD);  // addi
    }
    break;
  case 15:  // addis
    regimmop(d, a, false, (u32)inst.SIMM_16 << 16, Add, &XEmitter::ADD);
    break;
  case 24:  // ori
  case 25:  // oris
  {
    // check for nop
    if (a == s && inst.UIMM == 0)
    {
      // Make the nop visible in the generated code. not much use but interesting if we see one.
      NOP();
      return;
    }

    const u32 immediate = inst.OPCD == 24 ? inst.UIMM : inst.UIMM << 16;
    regimmop(a, s, true, immediate, Or, &XEmitter::OR);
    break;
  }
  case 28:  // andi
    regimmop(a, s, true, inst.UIMM, And, &XEmitter::AND, true);
    break;
  case 29:  // andis
    regimmop(a, s, true, inst.UIMM << 16, And, &XEmitter::AND, true);
    break;
  case 26:  // xori
  case 27:  // xoris
  {
    if (s == a && inst.UIMM == 0)
    {
      // Make the nop visible in the generated code.
      NOP();
      return;
    }

    const u32 immediate = inst.OPCD == 26 ? inst.UIMM : inst.UIMM << 16;
    regimmop(a, s, true, immediate, Xor, &XEmitter::XOR, false);
    break;
  }
  case 12:  // addic
    regimmop(d, a, false, (u32)(s32)inst.SIMM_16, Add, &XEmitter::ADD, false, true);
    break;
  case 13:  // addic_rc
    regimmop(d, a, true, (u32)(s32)inst.SIMM_16, Add, &XEmitter::ADD, true, true);
    break;
  default:
    FALLBACK_IF(true);
  }
}

bool Jit64::CheckMergedBranch(u32 crf) const
{
  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_MERGE))
    return false;

  if (!CanMergeNextInstructions(1))
    return false;

  const UGeckoInstruction& next = js.op[1].inst;
  return (((next.OPCD == 16 /* bcx */) ||
           ((next.OPCD == 19) && (next.SUBOP10 == 528) /* bcctrx */) ||
           ((next.OPCD == 19) && (next.SUBOP10 == 16) /* bclrx */)) &&
          (next.BO & BO_DONT_DECREMENT_FLAG) && !(next.BO & BO_DONT_CHECK_CONDITION) &&
          static_cast<u32>(next.BI >> 2) == crf);
}

void Jit64::DoMergedBranch()
{
  // Code that handles successful PPC branching.
  const UGeckoInstruction& next = js.op[1].inst;
  const u32 nextPC = js.op[1].address;

  if (js.op[1].branchIsIdleLoop)
  {
    ABI_PushRegistersAndAdjustStack({}, 0);
    ABI_CallFunction(CoreTiming::Idle);
    ABI_PopRegistersAndAdjustStack({}, 0);
    MOV(32, PPCSTATE(pc), Imm32(js.op[1].branchTo));
    WriteExceptionExit();
  }
  else if (next.OPCD == 16)  // bcx
  {
    if (next.LK)
      MOV(32, PPCSTATE(spr[SPR_LR]), Imm32(nextPC + 4));

    u32 destination;
    if (next.AA)
      destination = SignExt16(next.BD << 2);
    else
      destination = nextPC + SignExt16(next.BD << 2);
    WriteExit(destination, next.LK, nextPC + 4);
  }
  else if ((next.OPCD == 19) && (next.SUBOP10 == 528))  // bcctrx
  {
    if (next.LK)
      MOV(32, PPCSTATE(spr[SPR_LR]), Imm32(nextPC + 4));
    MOV(32, R(RSCRATCH), PPCSTATE(spr[SPR_CTR]));
    AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
    WriteExitDestInRSCRATCH(next.LK, nextPC + 4);
  }
  else if ((next.OPCD == 19) && (next.SUBOP10 == 16))  // bclrx
  {
    MOV(32, R(RSCRATCH), PPCSTATE(spr[SPR_LR]));
    if (!m_enable_blr_optimization)
      AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
    if (next.LK)
      MOV(32, PPCSTATE(spr[SPR_LR]), Imm32(nextPC + 4));
    WriteBLRExit();
  }
  else
  {
    PanicAlert("WTF invalid branch");
  }
}

void Jit64::DoMergedBranchCondition()
{
  js.downcountAmount++;
  js.skipInstructions = 1;
  const UGeckoInstruction& next = js.op[1].inst;
  int test_bit = 8 >> (next.BI & 3);
  bool condition = !!(next.BO & BO_BRANCH_IF_TRUE);
  const u32 nextPC = js.op[1].address;

  ASSERT(gpr.IsAllUnlocked());

  FixupBranch pDontBranch;
  if (test_bit & 8)
    pDontBranch = J_CC(condition ? CC_GE : CC_L, true);  // Test < 0, so jump over if >= 0.
  else if (test_bit & 4)
    pDontBranch = J_CC(condition ? CC_LE : CC_G, true);  // Test > 0, so jump over if <= 0.
  else if (test_bit & 2)
    pDontBranch = J_CC(condition ? CC_NE : CC_E, true);  // Test = 0, so jump over if != 0.
  else  // SO bit, do not branch (we don't emulate SO for cmp).
    pDontBranch = J(true);

  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();

    gpr.Flush();
    fpr.Flush();

    DoMergedBranch();
  }

  SetJumpTarget(pDontBranch);

  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
  {
    gpr.Flush();
    fpr.Flush();
    WriteExit(nextPC + 4);
  }
}

void Jit64::DoMergedBranchImmediate(s64 val)
{
  js.downcountAmount++;
  js.skipInstructions = 1;
  const UGeckoInstruction& next = js.op[1].inst;
  int test_bit = 8 >> (next.BI & 3);
  bool condition = !!(next.BO & BO_BRANCH_IF_TRUE);
  const u32 nextPC = js.op[1].address;

  ASSERT(gpr.IsAllUnlocked());

  bool branch;
  if (test_bit & 8)
    branch = condition ? val < 0 : val >= 0;
  else if (test_bit & 4)
    branch = condition ? val > 0 : val <= 0;
  else if (test_bit & 2)
    branch = condition ? val == 0 : val != 0;
  else  // SO bit, do not branch (we don't emulate SO for cmp).
    branch = false;

  if (branch)
  {
    gpr.Flush();
    fpr.Flush();
    DoMergedBranch();
  }
  else if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
  {
    gpr.Flush();
    fpr.Flush();
    WriteExit(nextPC + 4);
  }
}

void Jit64::cmpXX(UGeckoInstruction inst)
{
  // USES_CR
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int b = inst.RB;
  u32 crf = inst.CRFD;
  bool merge_branch = CheckMergedBranch(crf);

  bool signedCompare;
  RCOpArg comparand;
  switch (inst.OPCD)
  {
  // cmp / cmpl
  case 31:
    signedCompare = (inst.SUBOP10 == 0);
    comparand = signedCompare ? gpr.Use(b, RCMode::Read) : gpr.Bind(b, RCMode::Read);
    RegCache::Realize(comparand);
    break;

  // cmpli
  case 10:
    signedCompare = false;
    comparand = RCOpArg::Imm32((u32)inst.UIMM);
    break;

  // cmpi
  case 11:
    signedCompare = true;
    comparand = RCOpArg::Imm32((u32)(s32)(s16)inst.UIMM);
    break;

  default:
    signedCompare = false;  // silence compiler warning
    PanicAlert("cmpXX");
  }

  if (gpr.IsImm(a) && comparand.IsImm())
  {
    // Both registers contain immediate values, so we can pre-compile the compare result
    s64 compareResult = signedCompare ? (s64)gpr.SImm32(a) - (s64)comparand.SImm32() :
                                        (u64)gpr.Imm32(a) - (u64)comparand.Imm32();
    if (compareResult == (s32)compareResult)
    {
      MOV(64, PPCSTATE(cr.fields[crf]), Imm32((u32)compareResult));
    }
    else
    {
      MOV(64, R(RSCRATCH), Imm64(compareResult));
      MOV(64, PPCSTATE(cr.fields[crf]), R(RSCRATCH));
    }

    if (merge_branch)
    {
      RegCache::Unlock(comparand);
      DoMergedBranchImmediate(compareResult);
    }

    return;
  }

  if (!gpr.IsImm(a) && !signedCompare && comparand.IsImm() && comparand.Imm32() == 0)
  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::Read);
    RegCache::Realize(Ra);

    MOV(64, PPCSTATE(cr.fields[crf]), Ra);
    if (merge_branch)
    {
      TEST(64, Ra, Ra);
      RegCache::Unlock(comparand, Ra);
      DoMergedBranchCondition();
    }
    return;
  }

  const X64Reg input = RSCRATCH;
  if (gpr.IsImm(a))
  {
    if (signedCompare)
      MOV(64, R(input), Imm32(gpr.SImm32(a)));
    else
      MOV(32, R(input), Imm32(gpr.Imm32(a)));
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RegCache::Realize(Ra);
    if (signedCompare)
      MOVSX(64, 32, input, Ra);
    else
      MOVZX(64, 32, input, Ra);
  }

  if (comparand.IsImm())
  {
    // sign extension will ruin this, so store it in a register
    if (!signedCompare && (comparand.Imm32() & 0x80000000U) != 0)
    {
      MOV(32, R(RSCRATCH2), comparand);
      comparand = RCOpArg::R(RSCRATCH2);
    }
  }
  else
  {
    if (signedCompare)
    {
      MOVSX(64, 32, RSCRATCH2, comparand);
      comparand = RCOpArg::R(RSCRATCH2);
    }
  }

  if (comparand.IsImm() && comparand.Imm32() == 0)
  {
    MOV(64, PPCSTATE(cr.fields[crf]), R(input));
    // Place the comparison next to the branch for macro-op fusion
    if (merge_branch)
      TEST(64, R(input), R(input));
  }
  else
  {
    SUB(64, R(input), comparand);
    MOV(64, PPCSTATE(cr.fields[crf]), R(input));
  }

  if (merge_branch)
  {
    RegCache::Unlock(comparand);
    DoMergedBranchCondition();
  }
}

void Jit64::boolX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, s = inst.RS, b = inst.RB;
  bool needs_test = false;
  DEBUG_ASSERT_MSG(DYNA_REC, inst.OPCD == 31, "Invalid boolX");

  if (gpr.IsImm(s, b))
  {
    const u32 rs_offset = gpr.Imm32(s);
    const u32 rb_offset = gpr.Imm32(b);

    if (inst.SUBOP10 == 28)  // andx
      gpr.SetImmediate32(a, rs_offset & rb_offset);
    else if (inst.SUBOP10 == 476)  // nandx
      gpr.SetImmediate32(a, ~(rs_offset & rb_offset));
    else if (inst.SUBOP10 == 60)  // andcx
      gpr.SetImmediate32(a, rs_offset & (~rb_offset));
    else if (inst.SUBOP10 == 444)  // orx
      gpr.SetImmediate32(a, rs_offset | rb_offset);
    else if (inst.SUBOP10 == 124)  // norx
      gpr.SetImmediate32(a, ~(rs_offset | rb_offset));
    else if (inst.SUBOP10 == 412)  // orcx
      gpr.SetImmediate32(a, rs_offset | (~rb_offset));
    else if (inst.SUBOP10 == 316)  // xorx
      gpr.SetImmediate32(a, rs_offset ^ rb_offset);
    else if (inst.SUBOP10 == 284)  // eqvx
      gpr.SetImmediate32(a, ~(rs_offset ^ rb_offset));
  }
  else if (s == b)
  {
    if ((inst.SUBOP10 == 28 /* andx */) || (inst.SUBOP10 == 444 /* orx */))
    {
      if (a != s)
      {
        RCOpArg Rs = gpr.Use(s, RCMode::Read);
        RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
        RegCache::Realize(Rs, Ra);
        MOV(32, Ra, Rs);
      }
      else if (inst.Rc)
      {
        gpr.Bind(a, RCMode::Read).Realize();
      }
      needs_test = true;
    }
    else if ((inst.SUBOP10 == 476 /* nandx */) || (inst.SUBOP10 == 124 /* norx */))
    {
      if (a == s && !inst.Rc)
      {
        RCOpArg Ra = gpr.UseNoImm(a, RCMode::ReadWrite);
        RegCache::Realize(Ra);
        NOT(32, Ra);
      }
      else
      {
        RCOpArg Rs = gpr.Use(s, RCMode::Read);
        RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
        RegCache::Realize(Rs, Ra);
        MOV(32, Ra, Rs);
        NOT(32, Ra);
      }
      needs_test = true;
    }
    else if ((inst.SUBOP10 == 412 /* orcx */) || (inst.SUBOP10 == 284 /* eqvx */))
    {
      gpr.SetImmediate32(a, 0xFFFFFFFF);
    }
    else if ((inst.SUBOP10 == 60 /* andcx */) || (inst.SUBOP10 == 316 /* xorx */))
    {
      gpr.SetImmediate32(a, 0);
    }
    else
    {
      PanicAlert("WTF!");
    }
  }
  else if ((a == s) || (a == b))
  {
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCOpArg operand = gpr.Use(a == s ? b : s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
    RegCache::Realize(Rb, Rs, operand, Ra);

    if (inst.SUBOP10 == 28)  // andx
    {
      AND(32, Ra, operand);
    }
    else if (inst.SUBOP10 == 476)  // nandx
    {
      AND(32, Ra, operand);
      NOT(32, Ra);
      needs_test = true;
    }
    else if (inst.SUBOP10 == 60)  // andcx
    {
      if (cpu_info.bBMI1 && Rb.IsSimpleReg() && !Rs.IsImm())
      {
        ANDN(32, Ra, Rb.GetSimpleReg(), Rs);
      }
      else if (a == b)
      {
        NOT(32, Ra);
        AND(32, Ra, operand);
      }
      else
      {
        MOV(32, R(RSCRATCH), operand);
        NOT(32, R(RSCRATCH));
        AND(32, Ra, R(RSCRATCH));
      }
    }
    else if (inst.SUBOP10 == 444)  // orx
    {
      OR(32, Ra, operand);
    }
    else if (inst.SUBOP10 == 124)  // norx
    {
      OR(32, Ra, operand);
      NOT(32, Ra);
      needs_test = true;
    }
    else if (inst.SUBOP10 == 412)  // orcx
    {
      if (a == b)
      {
        NOT(32, Ra);
        OR(32, Ra, operand);
      }
      else
      {
        MOV(32, R(RSCRATCH), operand);
        NOT(32, R(RSCRATCH));
        OR(32, Ra, R(RSCRATCH));
      }
    }
    else if (inst.SUBOP10 == 316)  // xorx
    {
      XOR(32, Ra, operand);
    }
    else if (inst.SUBOP10 == 284)  // eqvx
    {
      NOT(32, Ra);
      XOR(32, Ra, operand);
    }
    else
    {
      PanicAlert("WTF");
    }
  }
  else
  {
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Rb, Rs, Ra);

    if (inst.SUBOP10 == 28)  // andx
    {
      MOV(32, Ra, Rs);
      AND(32, Ra, Rb);
    }
    else if (inst.SUBOP10 == 476)  // nandx
    {
      MOV(32, Ra, Rs);
      AND(32, Ra, Rb);
      NOT(32, Ra);
      needs_test = true;
    }
    else if (inst.SUBOP10 == 60)  // andcx
    {
      if (cpu_info.bBMI1 && Rb.IsSimpleReg() && !Rs.IsImm())
      {
        ANDN(32, Ra, Rb.GetSimpleReg(), Rs);
      }
      else
      {
        MOV(32, Ra, Rb);
        NOT(32, Ra);
        AND(32, Ra, Rs);
      }
    }
    else if (inst.SUBOP10 == 444)  // orx
    {
      MOV(32, Ra, Rs);
      OR(32, Ra, Rb);
    }
    else if (inst.SUBOP10 == 124)  // norx
    {
      MOV(32, Ra, Rs);
      OR(32, Ra, Rb);
      NOT(32, Ra);
      needs_test = true;
    }
    else if (inst.SUBOP10 == 412)  // orcx
    {
      MOV(32, Ra, Rb);
      NOT(32, Ra);
      OR(32, Ra, Rs);
    }
    else if (inst.SUBOP10 == 316)  // xorx
    {
      MOV(32, Ra, Rs);
      XOR(32, Ra, Rb);
    }
    else if (inst.SUBOP10 == 284)  // eqvx
    {
      MOV(32, Ra, Rs);
      NOT(32, Ra);
      XOR(32, Ra, Rb);
    }
    else
    {
      PanicAlert("WTF!");
    }
  }
  if (inst.Rc)
    ComputeRC(a, needs_test);
}

void Jit64::extsXx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, s = inst.RS;
  int size = inst.SUBOP10 == 922 ? 16 : 8;

  if (gpr.IsImm(s))
  {
    gpr.SetImmediate32(a, (u32)(s32)(size == 16 ? (s16)gpr.Imm32(s) : (s8)gpr.Imm32(s)));
  }
  else
  {
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Rs, Ra);
    MOVSX(32, size, Ra, Rs);
  }
  if (inst.Rc)
    ComputeRC(a);
}

void Jit64::subfic(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, d = inst.RD;

  RCOpArg Ra = gpr.Use(a, RCMode::Read);
  RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
  RegCache::Realize(Ra, Rd);

  int imm = inst.SIMM_16;
  if (d == a)
  {
    if (imm == 0)
    {
      // Flags act exactly like subtracting from 0
      NEG(32, Rd);
      // Output carry is inverted
      FinalizeCarry(CC_NC);
    }
    else if (imm == -1)
    {
      NOT(32, Rd);
      // CA is always set in this case
      FinalizeCarry(true);
    }
    else
    {
      NOT(32, Rd);
      ADD(32, Rd, Imm32(imm + 1));
      // Output carry is normal
      FinalizeCarry(CC_C);
    }
  }
  else
  {
    MOV(32, Rd, Imm32(imm));
    SUB(32, Rd, Ra);
    // Output carry is inverted
    FinalizeCarry(CC_NC);
  }
  // This instruction has no RC flag
}

void Jit64::subfx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, b = inst.RB, d = inst.RD;

  if (gpr.IsImm(a) && gpr.IsImm(b))
  {
    s32 i = gpr.SImm32(b), j = gpr.SImm32(a);
    gpr.SetImmediate32(d, i - j);
    if (inst.OE)
      GenerateConstantOverflow((s64)i - (s64)j);
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rb, Rd);

    if (d == b)
    {
      SUB(32, Rd, Ra);
    }
    else if (d == a)
    {
      MOV(32, R(RSCRATCH), Ra);
      MOV(32, Rd, Rb);
      SUB(32, Rd, R(RSCRATCH));
    }
    else
    {
      MOV(32, Rd, Rb);
      SUB(32, Rd, Ra);
    }
    if (inst.OE)
      GenerateOverflow();
  }
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::MultiplyImmediate(u32 imm, int a, int d, bool overflow)
{
  RCOpArg Ra = gpr.Use(a, RCMode::Read);
  RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
  RegCache::Realize(Ra, Rd);

  // simplest cases first
  if (imm == 0)
  {
    XOR(32, Rd, Rd);
    return;
  }

  if (imm == (u32)-1)
  {
    if (d != a)
      MOV(32, Rd, Ra);
    NEG(32, Rd);
    return;
  }

  // skip these if we need to check overflow flag
  if (!overflow)
  {
    // power of 2; just a shift
    if (MathUtil::IsPow2(imm))
    {
      u32 shift = IntLog2(imm);
      // use LEA if it saves an op
      if (d != a && shift <= 3 && shift >= 1 && Ra.IsSimpleReg())
      {
        LEA(32, Rd, MScaled(Ra.GetSimpleReg(), SCALE_1 << shift, 0));
      }
      else
      {
        if (d != a)
          MOV(32, Rd, Ra);
        if (shift)
          SHL(32, Rd, Imm8(shift));
      }
      return;
    }

    // We could handle factors of 2^N*3, 2^N*5, and 2^N*9 using lea+shl, but testing shows
    // it seems to be slower overall.
    static constexpr std::array<u8, 3> lea_scales{{3, 5, 9}};
    for (size_t i = 0; i < lea_scales.size(); i++)
    {
      if (imm == lea_scales[i] && Ra.IsSimpleReg())
      {
        LEA(32, Rd, MComplex(Ra.GetSimpleReg(), Ra.GetSimpleReg(), SCALE_2 << i, 0));
        return;
      }
    }
  }

  // if we didn't find any better options
  IMUL(32, Rd, Ra, Imm32(imm));
}

void Jit64::mulli(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, d = inst.RD;
  u32 imm = inst.SIMM_16;

  if (gpr.IsImm(a))
  {
    gpr.SetImmediate32(d, gpr.Imm32(a) * imm);
  }
  else
  {
    MultiplyImmediate(imm, a, d, false);
  }
}

void Jit64::mullwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, b = inst.RB, d = inst.RD;

  if (gpr.IsImm(a, b))
  {
    s32 i = gpr.SImm32(a), j = gpr.SImm32(b);
    gpr.SetImmediate32(d, i * j);
    if (inst.OE)
      GenerateConstantOverflow((s64)i * (s64)j);
  }
  else if (gpr.IsImm(a) || gpr.IsImm(b))
  {
    u32 imm = gpr.IsImm(a) ? gpr.Imm32(a) : gpr.Imm32(b);
    int src = gpr.IsImm(a) ? b : a;
    MultiplyImmediate(imm, src, d, inst.OE);
    if (inst.OE)
      GenerateOverflow();
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rb, Rd);

    if (d == a)
    {
      IMUL(32, Rd, Rb);
    }
    else if (d == b)
    {
      IMUL(32, Rd, Ra);
    }
    else
    {
      MOV(32, Rd, Rb);
      IMUL(32, Rd, Ra);
    }
    if (inst.OE)
      GenerateOverflow();
  }
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::mulhwXx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, b = inst.RB, d = inst.RD;
  bool sign = inst.SUBOP10 == 75;

  if (gpr.IsImm(a, b))
  {
    if (sign)
      gpr.SetImmediate32(d, (u32)((u64)(((s64)gpr.SImm32(a) * (s64)gpr.SImm32(b))) >> 32));
    else
      gpr.SetImmediate32(d, (u32)(((u64)gpr.Imm32(a) * (u64)gpr.Imm32(b)) >> 32));
  }
  else if (sign)
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.UseNoImm(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RCX64Reg eax = gpr.Scratch(EAX);
    RCX64Reg edx = gpr.Scratch(EDX);
    RegCache::Realize(Ra, Rb, Rd, eax, edx);

    MOV(32, eax, Ra);
    IMUL(32, Rb);
    MOV(32, Rd, edx);
  }
  else
  {
    // Not faster for signed because we'd need two movsx.
    // We need to bind everything to registers since the top 32 bits need to be zero.
    int src = d == b ? a : b;
    int other = src == b ? a : b;

    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RCX64Reg Rsrc = gpr.Bind(src, RCMode::Read);
    RCOpArg Rother = gpr.Use(other, RCMode::Read);
    RegCache::Realize(Rd, Rsrc, Rother);

    if (other != d)
      MOV(32, Rd, Rother);
    IMUL(64, Rd, Rsrc);
    SHR(64, Rd, Imm8(32));
  }
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::divwux(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, b = inst.RB, d = inst.RD;

  if (gpr.IsImm(a, b))
  {
    if (gpr.Imm32(b) == 0)
    {
      gpr.SetImmediate32(d, 0);
      if (inst.OE)
        GenerateConstantOverflow(true);
    }
    else
    {
      gpr.SetImmediate32(d, gpr.Imm32(a) / gpr.Imm32(b));
      if (inst.OE)
        GenerateConstantOverflow(false);
    }
  }
  else if (gpr.IsImm(b))
  {
    u32 divisor = gpr.Imm32(b);
    if (divisor == 0)
    {
      gpr.SetImmediate32(d, 0);
      if (inst.OE)
        GenerateConstantOverflow(true);
    }
    else
    {
      u32 shift = 31;
      while (!(divisor & (1 << shift)))
        shift--;

      if (divisor == (u32)(1 << shift))
      {
        RCOpArg Ra = gpr.Use(a, RCMode::Read);
        RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
        RegCache::Realize(Ra, Rd);

        if (d != a)
          MOV(32, Rd, Ra);
        if (shift)
          SHR(32, Rd, Imm8(shift));
      }
      else
      {
        u64 magic_dividend = 0x100000000ULL << shift;
        u32 magic = (u32)(magic_dividend / divisor);
        u32 max_quotient = magic >> shift;

        // Test for failure in round-up method
        if (((u64)(magic + 1) * (max_quotient * divisor - 1)) >> (shift + 32) != max_quotient - 1)
        {
          // If failed, use slower round-down method
          RCOpArg Ra = gpr.Use(a, RCMode::Read);
          RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
          RegCache::Realize(Ra, Rd);

          MOV(32, R(RSCRATCH), Imm32(magic));
          if (d != a)
            MOV(32, Rd, Ra);
          IMUL(64, Rd, R(RSCRATCH));
          ADD(64, Rd, R(RSCRATCH));
          SHR(64, Rd, Imm8(shift + 32));
        }
        else
        {
          // If success, use faster round-up method
          RCX64Reg Ra = gpr.Bind(a, RCMode::Read);
          RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
          RegCache::Realize(Ra, Rd);

          if (d == a)
          {
            MOV(32, R(RSCRATCH), Imm32(magic + 1));
            IMUL(64, Rd, R(RSCRATCH));
          }
          else
          {
            MOV(32, Rd, Imm32(magic + 1));
            IMUL(64, Rd, Ra);
          }
          SHR(64, Rd, Imm8(shift + 32));
        }
      }
      if (inst.OE)
        GenerateConstantOverflow(false);
    }
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCX64Reg Rb = gpr.Bind(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    // no register choice (do we need to do this?)
    RCX64Reg eax = gpr.Scratch(EAX);
    RCX64Reg edx = gpr.Scratch(EDX);
    RegCache::Realize(Ra, Rb, Rd, eax, edx);

    MOV(32, eax, Ra);
    XOR(32, edx, edx);
    TEST(32, Rb, Rb);
    FixupBranch not_div_by_zero = J_CC(CC_NZ);
    MOV(32, Rd, edx);
    if (inst.OE)
    {
      GenerateConstantOverflow(true);
    }
    FixupBranch end = J();
    SetJumpTarget(not_div_by_zero);
    DIV(32, Rb);
    MOV(32, Rd, eax);
    if (inst.OE)
    {
      GenerateConstantOverflow(false);
    }
    SetJumpTarget(end);
  }
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::divwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, b = inst.RB, d = inst.RD;

  if (gpr.IsImm(a, b))
  {
    s32 i = gpr.SImm32(a), j = gpr.SImm32(b);
    if (j == 0 || (i == (s32)0x80000000 && j == -1))
    {
      const u32 result = i < 0 ? 0xFFFFFFFF : 0x00000000;
      gpr.SetImmediate32(d, result);
      if (inst.OE)
        GenerateConstantOverflow(true);
    }
    else
    {
      gpr.SetImmediate32(d, i / j);
      if (inst.OE)
        GenerateConstantOverflow(false);
    }
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCX64Reg Rb = gpr.Bind(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    // no register choice
    RCX64Reg eax = gpr.Scratch(EAX);
    RCX64Reg edx = gpr.Scratch(EDX);
    RegCache::Realize(Ra, Rb, Rd, eax, edx);

    MOV(32, eax, Ra);
    TEST(32, Rb, Rb);
    const FixupBranch overflow = J_CC(CC_E);

    CMP(32, eax, Imm32(0x80000000));
    const FixupBranch normal_path1 = J_CC(CC_NE);

    CMP(32, Rb, Imm32(0xFFFFFFFF));
    const FixupBranch normal_path2 = J_CC(CC_NE);

    SetJumpTarget(overflow);
    SAR(32, eax, Imm8(31));
    MOV(32, Rd, eax);
    if (inst.OE)
    {
      GenerateConstantOverflow(true);
    }
    const FixupBranch done = J();

    SetJumpTarget(normal_path1);
    SetJumpTarget(normal_path2);

    CDQ();
    IDIV(32, Rb);
    MOV(32, Rd, eax);
    if (inst.OE)
    {
      GenerateConstantOverflow(false);
    }
    SetJumpTarget(done);
  }
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::addx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, b = inst.RB, d = inst.RD;

  if (gpr.IsImm(a, b))
  {
    s32 i = gpr.SImm32(a), j = gpr.SImm32(b);
    gpr.SetImmediate32(d, i + j);
    if (inst.OE)
      GenerateConstantOverflow((s64)i + (s64)j);
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rb, Rd);

    if (Ra.IsSimpleReg() && Rb.IsSimpleReg() && !inst.OE)
    {
      LEA(32, Rd, MRegSum(Ra.GetSimpleReg(), Rb.GetSimpleReg()));
    }
    else if (d == b)
    {
      ADD(32, Rd, Ra);
    }
    else
    {
      if (d != a)
        MOV(32, Rd, Ra);
      ADD(32, Rd, Rb);
    }
    if (inst.OE)
      GenerateOverflow();
  }
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::arithXex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  bool regsource = !(inst.SUBOP10 & 64);  // addex or subfex
  bool mex = !!(inst.SUBOP10 & 32);       // addmex/subfmex or addzex/subfzex
  bool add = !!(inst.SUBOP10 & 2);        // add or sub
  int a = inst.RA;
  int b = regsource ? inst.RB : a;
  int d = inst.RD;
  bool same_input_sub = !add && regsource && a == b;

  if (!js.carryFlagSet)
    JitGetAndClearCAOV(inst.OE);
  else
    UnlockFlags();

  bool invertedCarry = false;
  // Special case: subfe A, B, B is a common compiler idiom
  if (same_input_sub)
  {
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Rd);

    // Convert carry to borrow
    if (!js.carryFlagInverted)
      CMC();
    SBB(32, Rd, Rd);
    invertedCarry = true;
  }
  else if (!add && regsource && d == b)
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::ReadWrite);
    RegCache::Realize(Ra, Rd);

    if (!js.carryFlagInverted)
      CMC();
    SBB(32, Rd, Ra);
    invertedCarry = true;
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RCOpArg source =
        regsource ? gpr.Use(d == b ? a : b, RCMode::Read) : RCOpArg::Imm32(mex ? 0xFFFFFFFF : 0);
    RegCache::Realize(Ra, Rb, Rd, source);

    if (d != a && d != b)
      MOV(32, Rd, Ra);
    if (!add)
      NOT(32, Rd);
    // if the source is an immediate, we can invert carry by going from add -> sub and doing src =
    // -1 - src
    if (js.carryFlagInverted && source.IsImm())
    {
      SBB(32, Rd, Imm32(-1 - source.SImm32()));
      invertedCarry = true;
    }
    else
    {
      if (js.carryFlagInverted)
        CMC();
      ADC(32, Rd, source);
    }
  }
  FinalizeCarryOverflow(inst.OE, invertedCarry);
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::arithcx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  bool add = !!(inst.SUBOP10 & 2);  // add or sub
  int a = inst.RA, b = inst.RB, d = inst.RD;

  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rb, Rd);

    if (d == a && d != b)
    {
      if (add)
      {
        ADD(32, Rd, Rb);
      }
      else
      {
        // special case, because sub isn't reversible
        MOV(32, R(RSCRATCH), Ra);
        MOV(32, Rd, Rb);
        SUB(32, Rd, R(RSCRATCH));
      }
    }
    else
    {
      if (d != b)
        MOV(32, Rd, Rb);
      if (add)
        ADD(32, Rd, Ra);
      else
        SUB(32, Rd, Ra);
    }
  }

  FinalizeCarryOverflow(inst.OE, !add);
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::rlwinmx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int s = inst.RS;

  if (gpr.IsImm(s))
  {
    u32 result = gpr.Imm32(s);
    if (inst.SH != 0)
      result = Common::RotateLeft(result, inst.SH);
    result &= MakeRotationMask(inst.MB, inst.ME);
    gpr.SetImmediate32(a, result);
    if (inst.Rc)
      ComputeRC(a);
  }
  else
  {
    const bool left_shift = inst.SH && inst.MB == 0 && inst.ME == 31 - inst.SH;
    const bool right_shift = inst.SH && inst.ME == 31 && inst.MB == 32 - inst.SH;
    const u32 mask = MakeRotationMask(inst.MB, inst.ME);
    const bool simple_mask = mask == 0xff || mask == 0xffff;
    // In case of a merged branch, track whether or not we've set flags.
    // If not, we need to do a test later to get them.
    bool needs_test = true;
    // If we know the high bit can't be set, we can avoid doing a sign extend for flag storage.
    bool needs_sext = true;
    int mask_size = inst.ME - inst.MB + 1;

    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Rs, Ra);

    if (a != s && left_shift && Rs.IsSimpleReg() && inst.SH <= 3)
    {
      LEA(32, Ra, MScaled(Rs.GetSimpleReg(), SCALE_1 << inst.SH, 0));
    }
    // common optimized case: byte/word extract
    else if (simple_mask && !(inst.SH & (mask_size - 1)))
    {
      MOVZX(32, mask_size, Ra, Rs.ExtractWithByteOffset(inst.SH ? (32 - inst.SH) >> 3 : 0));
      needs_sext = false;
    }
    // another optimized special case: byte/word extract plus shift
    else if (((mask >> inst.SH) << inst.SH) == mask && !left_shift &&
             ((mask >> inst.SH) == 0xff || (mask >> inst.SH) == 0xffff))
    {
      MOVZX(32, mask_size, Ra, Rs);
      SHL(32, Ra, Imm8(inst.SH));
      needs_sext = inst.SH + mask_size >= 32;
    }
    else
    {
      if (a != s)
        MOV(32, Ra, Rs);

      if (left_shift)
      {
        SHL(32, Ra, Imm8(inst.SH));
      }
      else if (right_shift)
      {
        SHR(32, Ra, Imm8(inst.MB));
        needs_sext = false;
      }
      else
      {
        if (inst.SH != 0)
          ROL(32, Ra, Imm8(inst.SH));
        if (!(inst.MB == 0 && inst.ME == 31))
        {
          // we need flags if we're merging the branch
          if (inst.Rc && CheckMergedBranch(0))
            AND(32, Ra, Imm32(mask));
          else
            AndWithMask(Ra, mask);
          needs_sext = inst.MB == 0;
          needs_test = false;
        }
      }
    }

    Rs.Unlock();
    Ra.Unlock();

    if (inst.Rc)
      ComputeRC(a, needs_test, needs_sext);
  }
}

void Jit64::rlwimix(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int s = inst.RS;

  if (gpr.IsImm(a, s))
  {
    const u32 mask = MakeRotationMask(inst.MB, inst.ME);
    gpr.SetImmediate32(a,
                       (gpr.Imm32(a) & ~mask) | (Common::RotateLeft(gpr.Imm32(s), inst.SH) & mask));
    if (inst.Rc)
      ComputeRC(a);
  }
  else
  {
    const u32 mask = MakeRotationMask(inst.MB, inst.ME);
    bool needs_test = false;
    if (mask == 0 || (a == s && inst.SH == 0))
    {
      needs_test = true;
    }
    else if (mask == 0xFFFFFFFF)
    {
      RCOpArg Rs = gpr.Use(s, RCMode::Read);
      RCX64Reg Ra = gpr.Bind(a, RCMode::Read);
      RegCache::Realize(Rs, Ra);
      if (a != s)
        MOV(32, Ra, Rs);
      if (inst.SH)
        ROL(32, Ra, Imm8(inst.SH));
      needs_test = true;
    }
    else if (gpr.IsImm(s))
    {
      RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
      RegCache::Realize(Ra);
      AndWithMask(Ra, ~mask);
      OR(32, Ra, Imm32(Common::RotateLeft(gpr.Imm32(s), inst.SH) & mask));
    }
    else if (inst.SH)
    {
      bool isLeftShift = mask == 0U - (1U << inst.SH);
      bool isRightShift = mask == (1U << inst.SH) - 1;
      if (gpr.IsImm(a))
      {
        u32 maskA = gpr.Imm32(a) & ~mask;

        RCOpArg Rs = gpr.Use(s, RCMode::Read);
        RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
        RegCache::Realize(Rs, Ra);

        MOV(32, Ra, Rs);
        if (isLeftShift)
        {
          SHL(32, Ra, Imm8(inst.SH));
        }
        else if (isRightShift)
        {
          SHR(32, Ra, Imm8(32 - inst.SH));
        }
        else
        {
          ROL(32, Ra, Imm8(inst.SH));
          AND(32, Ra, Imm32(mask));
        }
        OR(32, Ra, Imm32(maskA));
      }
      else
      {
        // TODO: common cases of this might be faster with pinsrb or abuse of AH
        RCOpArg Rs = gpr.Use(s, RCMode::Read);
        RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
        RegCache::Realize(Rs, Ra);

        MOV(32, R(RSCRATCH), Rs);
        if (isLeftShift)
        {
          SHL(32, R(RSCRATCH), Imm8(inst.SH));
          AndWithMask(Ra, ~mask);
          OR(32, Ra, R(RSCRATCH));
        }
        else if (isRightShift)
        {
          SHR(32, R(RSCRATCH), Imm8(32 - inst.SH));
          AndWithMask(Ra, ~mask);
          OR(32, Ra, R(RSCRATCH));
        }
        else
        {
          ROL(32, R(RSCRATCH), Imm8(inst.SH));
          XOR(32, R(RSCRATCH), Ra);
          AndWithMask(RSCRATCH, mask);
          XOR(32, Ra, R(RSCRATCH));
        }
      }
    }
    else
    {
      RCX64Reg Rs = gpr.Bind(s, RCMode::Read);
      RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
      RegCache::Realize(Rs, Ra);
      XOR(32, Ra, Rs);
      AndWithMask(Ra, ~mask);
      XOR(32, Ra, Rs);
    }
    if (inst.Rc)
      ComputeRC(a, needs_test);
  }
}

void Jit64::rlwnmx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA, b = inst.RB, s = inst.RS;

  const u32 mask = MakeRotationMask(inst.MB, inst.ME);
  if (gpr.IsImm(b, s))
  {
    gpr.SetImmediate32(a, Common::RotateLeft(gpr.Imm32(s), gpr.Imm32(b) & 0x1F) & mask);
  }
  else
  {
    RCX64Reg ecx = gpr.Scratch(ECX);  // no register choice
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(ecx, Ra, Rb, Rs);

    MOV(32, ecx, Rb);
    if (a != s)
    {
      MOV(32, Ra, Rs);
    }
    ROL(32, Ra, ecx);
    // we need flags if we're merging the branch
    if (inst.Rc && CheckMergedBranch(0))
      AND(32, Ra, Imm32(mask));
    else
      AndWithMask(Ra, mask);
  }
  if (inst.Rc)
    ComputeRC(a, false);
}

void Jit64::negx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int d = inst.RD;

  if (gpr.IsImm(a))
  {
    gpr.SetImmediate32(d, ~(gpr.Imm32(a)) + 1);
    if (inst.OE)
      GenerateConstantOverflow(gpr.Imm32(d) == 0x80000000);
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rd);

    if (a != d)
      MOV(32, Rd, Ra);
    NEG(32, Rd);
    if (inst.OE)
      GenerateOverflow();
  }
  if (inst.Rc)
    ComputeRC(d, false);
}

void Jit64::srwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int b = inst.RB;
  int s = inst.RS;

  if (gpr.IsImm(b, s))
  {
    u32 amount = gpr.Imm32(b);
    gpr.SetImmediate32(a, (amount & 0x20) ? 0 : (gpr.Imm32(s) >> (amount & 0x1f)));
  }
  else
  {
    RCX64Reg ecx = gpr.Scratch(ECX);  // no register choice
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(ecx, Ra, Rb, Rs);

    MOV(32, ecx, Rb);
    if (a != s)
      MOV(32, Ra, Rs);
    SHR(64, Ra, ecx);
  }
  // Shift of 0 doesn't update flags, so we need to test just in case
  if (inst.Rc)
    ComputeRC(a);
}

void Jit64::slwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int b = inst.RB;
  int s = inst.RS;

  if (gpr.IsImm(b, s))
  {
    u32 amount = gpr.Imm32(b);
    gpr.SetImmediate32(a, (amount & 0x20) ? 0 : gpr.Imm32(s) << (amount & 0x1f));
    if (inst.Rc)
      ComputeRC(a);
  }
  else
  {
    RCX64Reg ecx = gpr.Scratch(ECX);  // no register choice
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(ecx, Ra, Rb, Rs);

    MOV(32, ecx, Rb);
    if (a != s)
      MOV(32, Ra, Rs);
    SHL(64, Ra, ecx);
    if (inst.Rc)
    {
      AND(32, Ra, Ra);
      RegCache::Unlock(ecx, Ra, Rb, Rs);
      ComputeRC(a, false);
    }
    else
    {
      MOVZX(64, 32, Ra, Ra);
    }
  }
}

void Jit64::srawx(UGeckoInstruction inst)
{
  // USES_XER
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int b = inst.RB;
  int s = inst.RS;

  {
    RCX64Reg ecx = gpr.Scratch(ECX);  // no register choice
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(ecx, Ra, Rb, Rs);

    MOV(32, ecx, Rb);
    if (a != s)
      MOV(32, Ra, Rs);
    SHL(64, Ra, Imm8(32));
    SAR(64, Ra, ecx);
    if (js.op->wantsCA)
    {
      MOV(32, R(RSCRATCH), Ra);
      SHR(64, Ra, Imm8(32));
      TEST(32, Ra, R(RSCRATCH));
    }
    else
    {
      SHR(64, Ra, Imm8(32));
    }
    FinalizeCarry(CC_NZ);
  }
  if (inst.Rc)
    ComputeRC(a);
}

void Jit64::srawix(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int s = inst.RS;
  int amount = inst.SH;

  if (amount != 0)
  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Ra, Rs);

    if (!js.op->wantsCA)
    {
      if (a != s)
        MOV(32, Ra, Rs);
      SAR(32, Ra, Imm8(amount));
    }
    else
    {
      MOV(32, R(RSCRATCH), Rs);
      if (a != s)
        MOV(32, Ra, R(RSCRATCH));
      // some optimized common cases that can be done in slightly fewer ops
      if (amount == 1)
      {
        SHR(32, R(RSCRATCH), Imm8(31));  // sign
        AND(32, R(RSCRATCH), Ra);        // (sign && carry)
        SAR(32, Ra, Imm8(1));
        MOV(8, PPCSTATE(xer_ca),
            R(RSCRATCH));  // XER.CA = sign && carry, aka (input&0x80000001) == 0x80000001
      }
      else
      {
        SAR(32, Ra, Imm8(amount));
        SHL(32, R(RSCRATCH), Imm8(32 - amount));
        TEST(32, R(RSCRATCH), Ra);
        FinalizeCarry(CC_NZ);
      }
    }
  }
  else
  {
    FinalizeCarry(false);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Ra, Rs);

    if (a != s)
      MOV(32, Ra, Rs);
  }
  if (inst.Rc)
    ComputeRC(a);
}

// count leading zeroes
void Jit64::cntlzwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  int a = inst.RA;
  int s = inst.RS;
  bool needs_test = false;

  if (gpr.IsImm(s))
  {
    u32 mask = 0x80000000;
    u32 i = 0;
    for (; i < 32; i++, mask >>= 1)
    {
      if (gpr.Imm32(s) & mask)
        break;
    }
    gpr.SetImmediate32(a, i);
  }
  else
  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Ra, Rs);

    if (cpu_info.bLZCNT)
    {
      LZCNT(32, Ra, Rs);
      needs_test = true;
    }
    else
    {
      BSR(32, Ra, Rs);
      FixupBranch gotone = J_CC(CC_NZ);
      MOV(32, Ra, Imm32(63));
      SetJumpTarget(gotone);
      XOR(32, Ra, Imm8(0x1f));  // flip order
    }
  }

  if (inst.Rc)
    ComputeRC(a, needs_test, false);
}

void Jit64::twX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);

  s32 a = inst.RA;

  if (inst.OPCD == 3)  // twi
  {
    RCOpArg Ra = gpr.UseNoImm(a, RCMode::Read);
    RegCache::Realize(Ra);
    CMP(32, Ra, Imm32((s32)(s16)inst.SIMM_16));
  }
  else  // tw
  {
    s32 b = inst.RB;
    RCX64Reg Ra = gpr.Bind(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RegCache::Realize(Ra, Rb);
    CMP(32, Ra, Rb);
  }

  constexpr std::array<CCFlags, 5> conditions{{CC_A, CC_B, CC_E, CC_G, CC_L}};
  std::vector<FixupBranch> fixups;

  for (size_t i = 0; i < conditions.size(); i++)
  {
    if (inst.TO & (1 << i))
    {
      FixupBranch f = J_CC(conditions[i], true);
      fixups.push_back(f);
    }
  }
  FixupBranch dont_trap = J();

  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();

    for (const FixupBranch& fixup : fixups)
    {
      SetJumpTarget(fixup);
    }
    LOCK();
    OR(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_PROGRAM));

    gpr.Flush();
    fpr.Flush();

    WriteExceptionExit();
  }

  SetJumpTarget(dont_trap);

  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
  {
    gpr.Flush();
    fpr.Flush();
    WriteExit(js.compilerPC + 4);
  }
}
