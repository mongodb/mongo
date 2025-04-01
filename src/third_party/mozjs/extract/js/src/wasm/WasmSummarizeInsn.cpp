/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "wasm/WasmSummarizeInsn.h"

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

namespace js {
namespace wasm {

// Sources of documentation of instruction-set encoding:
//
// Documentation for the ARM instruction sets can be found at
// https://developer.arm.com/documentation/ddi0487/latest.  The documentation
// is vast -- more than 10000 pages.  When looking up an instruction, be sure
// to look in the correct section for the target word size -- AArch64 (arm64)
// and AArch32 (arm32) instructions are listed in different sections.  And for
// AArch32, be sure to look only at the "A<digit> variant/encoding" and not at
// the "T<digit>" ones.  The latter are for Thumb encodings, which we don't
// generate.
//
// The Intel documentation is similarly comprehensive: search for "Intel® 64
// and IA-32 Architectures Software Developer’s Manual Combined Volumes: 1,
// 2A, 2B, 2C, 2D, 3A, 3B, 3C, 3D, and 4".  It's easy to find.

#if defined(DEBUG)

// ===================================================== x86_32 and x86_64 ====

#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)

// Returns true iff a "Mod R/M" byte indicates a memory transaction.
static bool ModRMisM(uint8_t modrm) {
  return (modrm & 0b11'000'000) != 0b11'000'000;
}

// Returns bits 6:4 of a Mod R/M byte, which (for our very limited purposes)
// is sometimes interpreted as an opcode extension.
static uint8_t ModRMmid3(uint8_t modrm) { return (modrm >> 3) & 0b00000111; }

// Some simple helpers for dealing with (bitsets of) instruction prefixes.
enum Prefix : uint32_t {
  PfxLock = 1 << 0,
  Pfx66 = 1 << 1,
  PfxF2 = 1 << 2,
  PfxF3 = 1 << 3,
  PfxRexW = 1 << 4,
  PfxVexL = 1 << 5
};
static bool isEmpty(uint32_t set) { return set == 0; }
static bool hasAllOf(uint32_t set, uint32_t mustBePresent) {
  return (set & mustBePresent) == mustBePresent;
}
static bool hasNoneOf(uint32_t set, uint32_t mustNotBePresent) {
  return (set & mustNotBePresent) == 0;
}
static bool hasOnly(uint32_t set, uint32_t onlyTheseMayBePresent) {
  return (set & ~onlyTheseMayBePresent) == 0;
}

// Implied opcode-escape prefixes; these are used for decoding VEX-prefixed
// (AVX) instructions only.  This is a real enumeration, not a bitset.
enum Escape { EscNone, Esc0F, Esc0F38, Esc0F3A };

Maybe<TrapMachineInsn> SummarizeTrapInstruction(const uint8_t* insn) {
  const bool is64bit = sizeof(void*) == 8;

  // First off, use up the prefix bytes, so we wind up pointing `insn` at the
  // primary opcode byte, and at the same time accumulate the prefixes in
  // `prefixes`.
  uint32_t prefixes = 0;

  bool hasREX = false;
  bool hasVEX = false;

  // Parse the "legacy" prefixes (only those we care about).  Skip REX on
  // 32-bit x86.
  while (true) {
    if (insn[0] >= 0x40 && insn[0] <= 0x4F && is64bit) {
      hasREX = true;
      // It has a REX prefix, but is REX.W set?
      if (insn[0] >= 0x48) {
        prefixes |= PfxRexW;
      }
      insn++;
      continue;
    }
    if (insn[0] == 0x66) {
      prefixes |= Pfx66;
      insn++;
      continue;
    }
    if (insn[0] == 0xF0) {
      prefixes |= PfxLock;
      insn++;
      continue;
    }
    if (insn[0] == 0xF2) {
      prefixes |= PfxF2;
      insn++;
      continue;
    }
    if (insn[0] == 0xF3) {
      prefixes |= PfxF3;
      insn++;
      continue;
    }
    if (insn[0] == 0xC4 || insn[0] == 0xC5) {
      hasVEX = true;
      // And fall through to the `break`, leaving `insn` pointing at the start
      // of the VEX prefix.
    }
    break;
  }

  // Throw out some invalid prefix combinations.
  if (hasAllOf(prefixes, PfxF2 | PfxF3) || (hasREX && hasVEX)) {
    return Nothing();
  }

  if (!hasVEX) {
    // The instruction has legacy prefixes only.  Deal with all these cases
    // first.

    // Determine the data size (in bytes) for "standard form" non-SIMD, non-FP
    // instructions whose opcode byte(s) don't directly imply an 8-bit
    // operation.  If both REX.W and 0x66 are present then REX.W "wins".
    int opSize = 4;
    if (prefixes & Pfx66) {
      opSize = 2;
    }
    if (prefixes & PfxRexW) {
      MOZ_ASSERT(is64bit);
      opSize = 8;
    }

    // `insn` should now point at the primary opcode, at least for the cases
    // we care about.  Start identifying instructions.  The OP_/OP2_/OP3_
    // comments are references to names as declared in Encoding-x86-shared.h.

    // This is the most common trap insn, so deal with it early.  Created by
    // MacroAssembler::wasmTrapInstruction.
    // OP_2BYTE_ESCAPE OP2_UD2
    // 0F 0B = ud2
    if (insn[0] == 0x0F && insn[1] == 0x0B && isEmpty(prefixes)) {
      return Some(TrapMachineInsn::OfficialUD);
    }

    // ==== Atomics

    // This is something of a kludge, but .. if the insn has a LOCK prefix,
    // declare it to be TrapMachineInsn::Atomic, regardless of what it
    // actually does.  It's good enough for checking purposes.
    if (prefixes & PfxLock) {
      return Some(TrapMachineInsn::Atomic);
    }
    // After this point, we can assume that no instruction has a lock prefix.

    // OP_XCHG_GbEb
    // OP_XCHG_GvEv
    // 86 = XCHG reg8, reg8/mem8.
    // 87 = XCHG reg64/32/16, reg64/32/16 / mem64/32/16.
    // The memory variants are atomic even though there is no LOCK prefix.
    if (insn[0] == 0x86 && ModRMisM(insn[1]) && isEmpty(prefixes)) {
      return Some(TrapMachineInsn::Atomic);
    }
    if (insn[0] == 0x87 && ModRMisM(insn[1]) &&
        hasOnly(prefixes, Pfx66 | PfxRexW)) {
      return Some(TrapMachineInsn::Atomic);
    }

    // ==== Scalar loads and stores

    // OP_MOV_EbGv
    // OP_MOV_GvEb
    // 88 = MOV src=reg, dst=mem/reg (8 bit int only)
    // 8A = MOV src=mem/reg, dst=reg (8 bit int only)
    if ((insn[0] == 0x88 || insn[0] == 0x8A) && ModRMisM(insn[1]) &&
        isEmpty(prefixes)) {
      return Some(insn[0] == 0x88 ? TrapMachineInsn::Store8
                                  : TrapMachineInsn::Load8);
    }

    // OP_MOV_EvGv
    // OP_MOV_GvEv
    // 89 = MOV src=reg, dst=mem/reg (64/32/16 bit int only)
    // 8B = MOV src=mem/reg, dst=reg (64/32/16 bit int only)
    if ((insn[0] == 0x89 || insn[0] == 0x8B) && ModRMisM(insn[1]) &&
        hasOnly(prefixes, Pfx66 | PfxRexW)) {
      return Some(insn[0] == 0x89 ? TrapMachineInsnForStore(opSize)
                                  : TrapMachineInsnForLoad(opSize));
    }

    // OP_GROUP11_EvIb GROUP11_MOV
    // C6 /0 = MOV src=immediate, dst=mem (8 bit int only)
    if (insn[0] == 0xC6 && ModRMisM(insn[1]) && ModRMmid3(insn[1]) == 0 &&
        isEmpty(prefixes)) {
      return Some(TrapMachineInsn::Store8);
    }
    // OP_GROUP11_EvIz GROUP11_MOV
    // C7 /0 = MOV src=immediate, dst=mem (64/32/16 bit int only)
    if (insn[0] == 0xC7 && ModRMisM(insn[1]) && ModRMmid3(insn[1]) == 0 &&
        hasOnly(prefixes, Pfx66 | PfxRexW)) {
      return Some(TrapMachineInsnForStore(opSize));
    }

    // OP_2BYTE_ESCAPE OP2_MOVZX_GvEb
    // OP_2BYTE_ESCAPE OP2_MOVSX_GvEb
    // 0F B6 = MOVZB{W,L,Q} src=reg/mem, dst=reg (8 -> 16, 32 or 64, int only)
    // 0F BE = MOVSB{W,L,Q} src=reg/mem, dst=reg (8 -> 16, 32 or 64, int only)
    if (insn[0] == 0x0F && (insn[1] == 0xB6 || insn[1] == 0xBE) &&
        ModRMisM(insn[2]) && (opSize == 2 || opSize == 4 || opSize == 8) &&
        hasOnly(prefixes, Pfx66 | PfxRexW)) {
      return Some(TrapMachineInsn::Load8);
    }
    // OP_2BYTE_ESCAPE OP2_MOVZX_GvEw
    // OP_2BYTE_ESCAPE OP2_MOVSX_GvEw
    // 0F B7 = MOVZW{L,Q} src=reg/mem, dst=reg (16 -> 32 or 64, int only)
    // 0F BF = MOVSW{L,Q} src=reg/mem, dst=reg (16 -> 32 or 64, int only)
    if (insn[0] == 0x0F && (insn[1] == 0xB7 || insn[1] == 0xBF) &&
        ModRMisM(insn[2]) && (opSize == 4 || opSize == 8) &&
        hasOnly(prefixes, PfxRexW)) {
      return Some(TrapMachineInsn::Load16);
    }

    // OP_MOVSXD_GvEv
    // REX.W 63 = MOVSLQ src=reg32/mem32, dst=reg64
    if (hasAllOf(prefixes, PfxRexW) && insn[0] == 0x63 && ModRMisM(insn[1]) &&
        hasOnly(prefixes, Pfx66 | PfxRexW)) {
      return Some(TrapMachineInsn::Load32);
    }

    // ==== SSE{2,3,E3,4} insns

    // OP_2BYTE_ESCAPE OP2_MOVPS_VpsWps
    // OP_2BYTE_ESCAPE OP2_MOVPS_WpsVps
    // 0F 10 = MOVUPS src=xmm/mem128, dst=xmm
    // 0F 11 = MOVUPS src=xmm, dst=xmm/mem128
    if (insn[0] == 0x0F && (insn[1] == 0x10 || insn[1] == 0x11) &&
        ModRMisM(insn[2]) && hasOnly(prefixes, PfxRexW)) {
      return Some(insn[1] == 0x10 ? TrapMachineInsn::Load128
                                  : TrapMachineInsn::Store128);
    }

    // OP_2BYTE_ESCAPE OP2_MOVLPS_VqEq
    // OP_2BYTE_ESCAPE OP2_MOVHPS_VqEq
    // 0F 12 = MOVLPS src=xmm64/mem64, dst=xmm
    // 0F 16 = MOVHPS src=xmm64/mem64, dst=xmm
    if (insn[0] == 0x0F && (insn[1] == 0x12 || insn[1] == 0x16) &&
        ModRMisM(insn[2]) && hasOnly(prefixes, PfxRexW)) {
      return Some(TrapMachineInsn::Load64);
    }

    // OP_2BYTE_ESCAPE OP2_MOVLPS_EqVq
    // OP_2BYTE_ESCAPE OP2_MOVHPS_EqVq
    // 0F 13 = MOVLPS src=xmm64, dst=xmm64/mem64
    // 0F 17 = MOVHPS src=xmm64, dst=xmm64/mem64
    if (insn[0] == 0x0F && (insn[1] == 0x13 || insn[1] == 0x17) &&
        ModRMisM(insn[2]) && hasOnly(prefixes, PfxRexW)) {
      return Some(TrapMachineInsn::Store64);
    }

    // PRE_SSE_F2 OP_2BYTE_ESCAPE OP2_MOVSD_VsdWsd
    // PRE_SSE_F2 OP_2BYTE_ESCAPE OP2_MOVSD_WsdVsd
    // F2 0F 10 = MOVSD src=mem64/xmm64, dst=xmm64
    // F2 0F 11 = MOVSD src=xmm64, dst=mem64/xmm64
    if (hasAllOf(prefixes, PfxF2) && insn[0] == 0x0F &&
        (insn[1] == 0x10 || insn[1] == 0x11) && ModRMisM(insn[2]) &&
        hasOnly(prefixes, PfxRexW | PfxF2)) {
      return Some(insn[1] == 0x10 ? TrapMachineInsn::Load64
                                  : TrapMachineInsn::Store64);
    }

    // PRE_SSE_F2 OP_2BYTE_ESCAPE OP2_MOVDDUP_VqWq
    // F2 0F 12 = MOVDDUP src=mem64/xmm64, dst=xmm
    if (hasAllOf(prefixes, PfxF2) && insn[0] == 0x0F && insn[1] == 0x12 &&
        ModRMisM(insn[2]) && hasOnly(prefixes, PfxF2)) {
      return Some(TrapMachineInsn::Load64);
    }

    // PRE_SSE_F3 OP_2BYTE_ESCAPE OP2_MOVSS_VssWss (name does not exist)
    // PRE_SSE_F3 OP_2BYTE_ESCAPE OP2_MOVSS_WssVss (name does not exist)
    // F3 0F 10 = MOVSS src=mem32/xmm32, dst=xmm32
    // F3 0F 11 = MOVSS src=xmm32, dst=mem32/xmm32
    if (hasAllOf(prefixes, PfxF3) && insn[0] == 0x0F &&
        (insn[1] == 0x10 || insn[1] == 0x11) && ModRMisM(insn[2]) &&
        hasOnly(prefixes, PfxRexW | PfxF3)) {
      return Some(insn[1] == 0x10 ? TrapMachineInsn::Load32
                                  : TrapMachineInsn::Store32);
    }

    // PRE_SSE_F3 OP_2BYTE_ESCAPE OP2_MOVDQ_VdqWdq
    // PRE_SSE_F3 OP_2BYTE_ESCAPE OP2_MOVDQ_WdqVdq
    // F3 0F 6F = MOVDQU src=mem128/xmm, dst=xmm
    // F3 0F 7F = MOVDQU src=xmm, dst=mem128/xmm
    if (hasAllOf(prefixes, PfxF3) && insn[0] == 0x0F &&
        (insn[1] == 0x6F || insn[1] == 0x7F) && ModRMisM(insn[2]) &&
        hasOnly(prefixes, PfxF3)) {
      return Some(insn[1] == 0x6F ? TrapMachineInsn::Load128
                                  : TrapMachineInsn::Store128);
    }

    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_3A OP3_PINSRB_VdqEvIb
    // 66 0F 3A 20 /r ib = PINSRB $imm8, src=mem8/ireg8, dst=xmm128.  I'd guess
    // that REX.W is meaningless here and therefore we should exclude it.
    if (hasAllOf(prefixes, Pfx66) && insn[0] == 0x0F && insn[1] == 0x3A &&
        insn[2] == 0x20 && ModRMisM(insn[3]) && hasOnly(prefixes, Pfx66)) {
      return Some(TrapMachineInsn::Load8);
    }
    // PRE_SSE_66 OP_2BYTE_ESCAPE OP2_PINSRW
    // 66 0F C4 /r ib = PINSRW $imm8, src=mem16/ireg16, dst=xmm128.  REX.W is
    // probably meaningless here.
    if (hasAllOf(prefixes, Pfx66) && insn[0] == 0x0F && insn[1] == 0xC4 &&
        ModRMisM(insn[2]) && hasOnly(prefixes, Pfx66)) {
      return Some(TrapMachineInsn::Load16);
    }
    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_3A OP3_INSERTPS_VpsUps
    // 66 0F 3A 21 /r ib = INSERTPS $imm8, src=mem32/xmm32, dst=xmm128.
    // REX.W is probably meaningless here.
    if (hasAllOf(prefixes, Pfx66) && insn[0] == 0x0F && insn[1] == 0x3A &&
        insn[2] == 0x21 && ModRMisM(insn[3]) && hasOnly(prefixes, Pfx66)) {
      return Some(TrapMachineInsn::Load32);
    }

    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_3A OP3_PEXTRB_EvVdqIb
    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_3A OP3_PEXTRW_EwVdqIb
    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_3A OP3_EXTRACTPS_EdVdqIb
    // 66 0F 3A 14 /r ib = PEXTRB src=xmm8, dst=reg8/mem8
    // 66 0F 3A 15 /r ib = PEXTRW src=xmm16, dst=reg16/mem16
    // 66 0F 3A 17 /r ib = EXTRACTPS src=xmm32, dst=reg32/mem32
    // REX.W is probably meaningless here.
    if (hasAllOf(prefixes, Pfx66) && insn[0] == 0x0F && insn[1] == 0x3A &&
        (insn[2] == 0x14 || insn[2] == 0x15 || insn[2] == 0x17) &&
        ModRMisM(insn[3]) && hasOnly(prefixes, Pfx66)) {
      return Some(insn[2] == 0x14   ? TrapMachineInsn::Store8
                  : insn[2] == 0x15 ? TrapMachineInsn::Store16
                                    : TrapMachineInsn::Store32);
    }

    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_38 OP3_PMOVSXBW_VdqWdq
    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_38 OP3_PMOVSXWD_VdqWdq
    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_38 OP3_PMOVSXDQ_VdqWdq
    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_38 OP3_PMOVZXBW_VdqWdq
    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_38 OP3_PMOVZXWD_VdqWdq
    // PRE_SSE_66 OP_2BYTE_ESCAPE ESCAPE_38 OP3_PMOVZXDQ_VdqWdq
    // 66 0F 38 20 /r = PMOVSXBW src=mem64/xmm64, dst=xmm
    // 66 0F 38 23 /r = PMOVSXWD src=mem64/xmm64, dst=xmm
    // 66 0F 38 25 /r = PMOVSXDQ src=mem64/xmm64, dst=xmm
    // 66 0F 38 30 /r = PMOVZXBW src=mem64/xmm64, dst=xmm
    // 66 0F 38 33 /r = PMOVZXWD src=mem64/xmm64, dst=xmm
    // 66 0F 38 35 /r = PMOVZXDQ src=mem64/xmm64, dst=xmm
    if (hasAllOf(prefixes, Pfx66) && insn[0] == 0x0F && insn[1] == 0x38 &&
        (insn[2] == 0x20 || insn[2] == 0x23 || insn[2] == 0x25 ||
         insn[2] == 0x30 || insn[2] == 0x33 || insn[2] == 0x35) &&
        ModRMisM(insn[3]) && hasOnly(prefixes, Pfx66)) {
      return Some(TrapMachineInsn::Load64);
    }

    // The insn only has legacy prefixes, and was not identified.
    return Nothing();
  }

  // We're dealing with a VEX-prefixed insn.  Fish out relevant bits of the
  // VEX prefix.  VEX prefixes come in two kinds: a 3-byte prefix, first byte
  // 0xC4, which gives us 16 bits of extra data, and a 2-byte prefix, first
  // byte 0xC5, which gives us 8 bits of extra data.  The 2-byte variant
  // contains a subset of the data that the 3-byte variant does and
  // (presumably) is to be used when the default values of the omitted fields
  // are correct for the instruction that is encoded.
  //
  // An instruction can't have both VEX and REX prefixes, because a 3-byte VEX
  // prefix specifies everything a REX prefix does, that is, the four bits
  // REX.{WRXB} and allowing both to be present would allow conflicting values
  // for them.  Of these four bits, we only care about REX.W (as obtained here
  // from the VEX prefix).
  //
  // A VEX prefix can also specify (imply?) the presence of the legacy
  // prefixes 66, F2 and F3.  Although the byte sequence we will have parsed
  // for this insn doesn't actually contain any of those, we must decode as if
  // we had seen them as legacy prefixes.
  //
  // A VEX prefix can also specify (imply?) the presence of the opcode escape
  // byte sequences 0F, 0F38 and 0F3A.  These are collected up into `esc`.
  // Again, we must decode as if we had actually seen these, although we
  // haven't really.
  //
  // The VEX prefix also holds various other bits which we ignore, because
  // these specify details of registers etc which we don't care about.
  MOZ_ASSERT(hasVEX && !hasREX);
  MOZ_ASSERT(hasNoneOf(prefixes, PfxRexW));
  MOZ_ASSERT(insn[0] == 0xC4 || insn[0] == 0xC5);

  Escape esc = EscNone;

  if (insn[0] == 0xC4) {
    // This is a 3-byte VEX prefix (3 bytes including the 0xC4).
    switch (insn[1] & 0x1F) {
      case 1:
        esc = Esc0F;
        break;
      case 2:
        esc = Esc0F38;
        break;
      case 3:
        esc = Esc0F3A;
        break;
      default:
        return Nothing();
    }
    switch (insn[2] & 3) {
      case 0:
        break;
      case 1:
        prefixes |= Pfx66;
        break;
      case 2:
        prefixes |= PfxF3;
        break;
      case 3:
        prefixes |= PfxF2;
        break;
    }
    if (insn[2] & 4) {
      // VEX.L distinguishes 128-bit (VEX.L==0) from 256-bit (VEX.L==1)
      // operations.
      prefixes |= PfxVexL;
    }
    if ((insn[2] & 0x80) && is64bit) {
      // Pull out REX.W, but only on 64-bit targets.  We'll need it for insn
      // decoding.  Recall that REX.W == 1 basically means "the
      // integer-register (GPR) aspect of this instruction requires a 64-bit
      // transaction", so we expect these to be relatively rare, since VEX is
      // primary used for SIMD instructions.
      prefixes |= PfxRexW;
    }
    // Step forwards to the primary opcode byte
    insn += 3;
  } else if (insn[0] == 0xC5) {
    // This is a 2-byte VEX prefix (2 bytes including the 0xC5).  Since it has
    // only 8 bits of useful payload, it adds less information than an 0xC4
    // prefix.
    esc = Esc0F;
    switch (insn[1] & 3) {
      case 0:
        break;
      case 1:
        prefixes |= Pfx66;
        break;
      case 2:
        prefixes |= PfxF3;
        break;
      case 3:
        prefixes |= PfxF2;
        break;
    }
    if (insn[1] & 4) {
      prefixes |= PfxVexL;
    }
    insn += 2;
  }

  // This isn't allowed.
  if (hasAllOf(prefixes, PfxF2 | PfxF3)) {
    return Nothing();
  }

  // This is useful for diagnosing decoding failures.
  // if (0) {
  //   fprintf(stderr, "FAIL  VEX  66=%d,F2=%d,F3=%d,REXW=%d,VEXL=%d esc=%s\n",
  //           (prefixes & Pfx66) ? 1 : 0, (prefixes & PfxF2) ? 1 : 0,
  //           (prefixes & PfxF3) ? 1 : 0, (prefixes & PfxRexW) ? 1 : 0,
  //           (prefixes & PfxVexL) ? 1 : 0,
  //           esc == Esc0F3A   ? "0F3A"
  //           : esc == Esc0F38 ? "0F38"
  //           : esc == Esc0F   ? "0F"
  //                            : "none");
  // }

  // (vex prefix) OP2_MOVPS_VpsWps
  // (vex prefix) OP2_MOVPS_WpsVps
  // 66=0,F2=0,F3=0,REXW=0,VEXL=0 esc=0F 10 = VMOVUPS src=xmm/mem128, dst=xmm
  // 66=0,F2=0,F3=0,REXW=0,VEXL=0 esc=0F 11 = VMOVUPS src=xmm, dst=xmm/mem128
  // REX.W is ignored.
  if (hasNoneOf(prefixes, Pfx66 | PfxF2 | PfxF3 | PfxRexW | PfxVexL) &&
      esc == Esc0F && (insn[0] == 0x10 || insn[0] == 0x11) &&
      ModRMisM(insn[1])) {
    return Some(insn[0] == 0x10 ? TrapMachineInsn::Load128
                                : TrapMachineInsn::Store128);
  }

  // (vex prefix) OP2_MOVSD_VsdWsd
  // (vex prefix) OP2_MOVSD_WsdVsd
  // 66=0,F2=1,F3=0,REXW=0,VEXL=0 esc=0F 10 = VMOVSD src=mem64, dst=xmm
  // 66=0,F2=1,F3=0,REXW=0,VEXL=0 esc=0F 11 = VMOVSD src=xmm, dst=mem64
  // REX.W and VEX.L are ignored.
  if (hasAllOf(prefixes, PfxF2) &&
      hasNoneOf(prefixes, Pfx66 | PfxF3 | PfxRexW | PfxVexL) && esc == Esc0F &&
      (insn[0] == 0x10 || insn[0] == 0x11) && ModRMisM(insn[1])) {
    return Some(insn[0] == 0x10 ? TrapMachineInsn::Load64
                                : TrapMachineInsn::Store64);
  }

  // (vex prefix) OP2_MOVSS_VssWss (name does not exist)
  // (vex prefix) OP2_MOVSS_WssVss (name does not exist)
  // 66=0,F2=0,F3=1,REXW=0,VEXL=0 esc=0F 10 = VMOVSS src=mem32, dst=xmm
  // 66=0,F2=0,F3=1,REXW=0,VEXL=0 esc=0F 11 = VMOVSS src=xmm, dst=mem32
  // REX.W and VEX.L are ignored.
  if (hasAllOf(prefixes, PfxF3) &&
      hasNoneOf(prefixes, Pfx66 | PfxF2 | PfxRexW | PfxVexL) && esc == Esc0F &&
      (insn[0] == 0x10 || insn[0] == 0x11) && ModRMisM(insn[1])) {
    return Some(insn[0] == 0x10 ? TrapMachineInsn::Load32
                                : TrapMachineInsn::Store32);
  }

  // (vex prefix) OP2_MOVDDUP_VqWq
  // 66=0,F2=1,F3=0,REXW=0,VEXL=0 esc=0F 12 = VMOVDDUP src=xmm/m64, dst=xmm
  // REX.W is ignored.
  if (hasAllOf(prefixes, PfxF2) &&
      hasNoneOf(prefixes, Pfx66 | PfxF3 | PfxRexW | PfxVexL) && esc == Esc0F &&
      insn[0] == 0x12 && ModRMisM(insn[1])) {
    return Some(TrapMachineInsn::Load64);
  }

  // (vex prefix) OP2_MOVLPS_EqVq
  // (vex prefix) OP2_MOVHPS_EqVq
  // 66=0,F2=0,F3=0,REXW=0,VEXL=0 esc=0F 13 = VMOVLPS src=xmm, dst=mem64
  // 66=0,F2=0,F3=0,REXW=0,VEXL=0 esc=0F 17 = VMOVHPS src=xmm, dst=mem64
  // REX.W is ignored.  These do a 64-bit mem transaction despite the 'S' in
  // the name, because a pair of float32s are transferred.
  if (hasNoneOf(prefixes, Pfx66 | PfxF2 | PfxF3 | PfxRexW | PfxVexL) &&
      esc == Esc0F && (insn[0] == 0x13 || insn[0] == 0x17) &&
      ModRMisM(insn[1])) {
    return Some(TrapMachineInsn::Store64);
  }

  // (vex prefix) OP2_MOVDQ_VdqWdq
  // (vex prefix) OP2_MOVDQ_WdqVdq
  // 66=0,F2=0,F3=1,REXW=0,VEXL=0 esc=0F 6F = VMOVDQU src=xmm/mem128, dst=xmm
  // 66=0,F2=0,F3=1,REXW=0,VEXL=0 esc=0F 7F = VMOVDQU src=xmm, dst=xmm/mem128
  // REX.W is ignored.
  if (hasAllOf(prefixes, PfxF3) &&
      hasNoneOf(prefixes, Pfx66 | PfxF2 | PfxRexW | PfxVexL) && esc == Esc0F &&
      (insn[0] == 0x6F || insn[0] == 0x7F) && ModRMisM(insn[1])) {
    return Some(insn[0] == 0x6F ? TrapMachineInsn::Load128
                                : TrapMachineInsn::Store128);
  }

  // (vex prefix) OP2_PINSRW
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=OF C4
  //                                   = PINSRW src=ireg/mem16,src=xmm,dst=xmm
  // REX.W is ignored.
  if (hasAllOf(prefixes, Pfx66) &&
      hasNoneOf(prefixes, PfxF2 | PfxF3 | PfxRexW | PfxVexL) && esc == Esc0F &&
      insn[0] == 0xC4 && ModRMisM(insn[1])) {
    return Some(TrapMachineInsn::Load16);
  }

  // (vex prefix) OP3_PMOVSXBW_VdqWdq
  // (vex prefix) OP3_PMOVSXWD_VdqWdq
  // (vex prefix) OP3_PMOVSXDQ_VdqWdq
  // (vex prefix) OP3_PMOVZXBW_VdqWdq
  // (vex prefix) OP3_PMOVZXWD_VdqWdq
  // (vex prefix) OP3_PMOVZXDQ_VdqWdq
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 20 = VPMOVSXBW src=xmm/m64, dst=xmm
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 23 = VPMOVSXWD src=xmm/m64, dst=xmm
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 25 = VPMOVSXDQ src=xmm/m64, dst=xmm
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 30 = VPMOVZXBW src=xmm/m64, dst=xmm
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 33 = VPMOVZXWD src=xmm/m64, dst=xmm
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 35 = VPMOVZXDQ src=xmm/m64, dst=xmm
  // REX.W is ignored.
  if (hasAllOf(prefixes, Pfx66) &&
      hasNoneOf(prefixes, PfxF2 | PfxF3 | PfxRexW | PfxVexL) &&
      esc == Esc0F38 &&
      (insn[0] == 0x20 || insn[0] == 0x23 || insn[0] == 0x25 ||
       insn[0] == 0x30 || insn[0] == 0x33 || insn[0] == 0x35) &&
      ModRMisM(insn[1])) {
    return Some(TrapMachineInsn::Load64);
  }

  // (vex prefix) OP3_VBROADCASTB_VxWx
  // (vex prefix) OP3_VBROADCASTW_VxWx
  // (vex prefix) OP3_VBROADCASTSS_VxWd
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 78
  //                                   = VPBROADCASTB src=xmm8/mem8, dst=xmm
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 79
  //                                   = VPBROADCASTW src=xmm16/mem16, dst=xmm
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F38 18
  //                                   = VBROADCASTSS src=m32, dst=xmm
  // VPBROADCASTB/W require REX.W == 0; VBROADCASTSS ignores REX.W.
  if (hasAllOf(prefixes, Pfx66) &&
      hasNoneOf(prefixes, PfxF2 | PfxF3 | PfxRexW | PfxVexL) &&
      esc == Esc0F38 &&
      (insn[0] == 0x78 || insn[0] == 0x79 || insn[0] == 0x18) &&
      ModRMisM(insn[1])) {
    return Some(insn[0] == 0x78   ? TrapMachineInsn::Load8
                : insn[0] == 0x79 ? TrapMachineInsn::Load16
                                  : TrapMachineInsn::Load32);
  }

  // (vex prefix) OP3_PEXTRB_EvVdqIb
  // (vex prefix) OP3_PEXTRW_EwVdqIb
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F3A 14 = VPEXTRB src=xmm, dst=ireg/mem8
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F3A 15 = VPEXTRW src=xmm, dst=ireg/mem16
  // These require REX.W == 0.
  if (hasAllOf(prefixes, Pfx66) &&
      hasNoneOf(prefixes, PfxF2 | PfxF3 | PfxRexW | PfxVexL) &&
      esc == Esc0F3A && (insn[0] == 0x14 || insn[0] == 0x15) &&
      ModRMisM(insn[1])) {
    return Some(insn[0] == 0x14 ? TrapMachineInsn::Store8
                                : TrapMachineInsn::Store16);
  }

  // (vex prefix) OP3_EXTRACTPS_EdVdqIb
  // 66=1,F2=0,F3=0,REXW=0,VEXL=0 esc=0F3A 17
  //                                   = VEXTRACTPS src=xmm, dst=ireg/mem32
  // REX.W is ignored.
  if (hasAllOf(prefixes, Pfx66) &&
      hasNoneOf(prefixes, PfxF2 | PfxF3 | PfxRexW | PfxVexL) &&
      esc == Esc0F3A && insn[0] == 0x17 && ModRMisM(insn[1])) {
    return Some(TrapMachineInsn::Store32);
  }

  // The instruction was not identified.
  return Nothing();
}

// ================================================================= arm64 ====

#  elif defined(JS_CODEGEN_ARM64)

Maybe<TrapMachineInsn> SummarizeTrapInstruction(const uint8_t* insnAddr) {
  // Check instruction alignment.
  MOZ_ASSERT(0 == (uintptr_t(insnAddr) & 3));

  const uint32_t insn = *(uint32_t*)insnAddr;

#    define INSN(_maxIx, _minIx) \
      ((insn >> (_minIx)) & ((uint32_t(1) << ((_maxIx) - (_minIx) + 1)) - 1))

  // MacroAssembler::wasmTrapInstruction uses this to create SIGILL.
  if (insn == 0xD4A00000) {
    return Some(TrapMachineInsn::OfficialUD);
  }

  // A note about loads and stores.  Many (perhaps all) integer loads and
  // stores use bits 31:30 of the instruction as a size encoding, thusly:
  //
  //   11 -> 64 bit, 10 -> 32 bit, 01 -> 16 bit, 00 -> 8 bit
  //
  // It is also very common for corresponding load and store instructions to
  // differ by exactly one bit (logically enough).
  //
  // Meaning of register names:
  //
  //   Xn      The n-th GPR (all 64 bits), for 0 <= n <= 31
  //   Xn|SP   The n-th GPR (all 64 bits), for 0 <= n <= 30, or SP when n == 31
  //   Wn      The lower 32 bits of the n-th GPR
  //   Qn      All 128 bits of the n-th SIMD register
  //   Dn      Lower 64 bits of the n-th SIMD register
  //   Sn      Lower 32 bits of the n-th SIMD register

  // Plain and zero-extending loads/stores, reg + offset, scaled
  switch (INSN(31, 22)) {
    // 11 111 00100 imm12 n t = STR Xt, [Xn|SP, #imm12 * 8]
    case 0b11'111'00100:
      return Some(TrapMachineInsn::Store64);
    // 10 111 00100 imm12 n t = STR Wt, [Xn|SP, #imm12 * 4]
    case 0b10'111'00100:
      return Some(TrapMachineInsn::Store32);
    // 01 111 00100 imm12 n t = STRH Wt, [Xn|SP, #imm12 * 2]
    case 0b01'111'00100:
      return Some(TrapMachineInsn::Store16);
    // 00 111 00100 imm12 n t = STRB Wt, [Xn|SP, #imm12 * 1]
    case 0b00'111'00100:
      return Some(TrapMachineInsn::Store8);
    // 11 111 00101 imm12 n t = LDR Xt, [Xn|SP, #imm12 * 8]
    case 0b11'111'00101:
      return Some(TrapMachineInsn::Load64);
    // 10 111 00101 imm12 n t = LDR Wt, [Xn|SP, #imm12 * 4]
    case 0b10'111'00101:
      return Some(TrapMachineInsn::Load32);
    // 01 111 00101 imm12 n t = LDRH Wt, [Xn|SP, #imm12 * 2]
    case 0b01'111'00101:
      return Some(TrapMachineInsn::Load16);
    // 00 111 00101 imm12 n t = LDRB Wt, [Xn|SP, #imm12 * 1]
    case 0b00'111'00101:
      return Some(TrapMachineInsn::Load8);
  }

  // Plain, sign- and zero-extending loads/stores, reg + offset, unscaled

  if (INSN(11, 10) == 0b00) {
    switch (INSN(31, 21)) {
      // 11 111 00001 0 imm9 00 n t = LDUR Xt, [Xn|SP, #imm9]
      case 0b11'111'00001'0:
        return Some(TrapMachineInsn::Load64);
      // 10 111 00001 0 imm9 00 n t = LDUR Wt, [Xn|SP, #imm9]
      case 0b10'111'00001'0:
        return Some(TrapMachineInsn::Load32);
      // 01 111 00001 0 imm9 00 n t = LDURH Wt, [Xn|SP, #imm9]
      case 0b01'111'00001'0:
        return Some(TrapMachineInsn::Load16);
      // We do have code to generate LDURB insns, but it appears to not be used.
      // 11 111 00000 0 imm9 00 n t = STUR Xt, [Xn|SP, #imm9]
      case 0b11'111'00000'0:
        return Some(TrapMachineInsn::Store64);
      // 10 111 00000 0 imm9 00 n t = STUR Wt, [Xn|SP, #imm9]
      case 0b10'111'00000'0:
        return Some(TrapMachineInsn::Store32);
      // 01 111 00000 0 imm9 00 n t = STURH Wt, [Xn|SP, #imm9]
      case 0b01'111'00000'0:
        return Some(TrapMachineInsn::Store16);
      // STURB missing?
      // Sign extending loads:
      // 10 111 000 10 0 imm9 00 n t = LDURSW Xt, [Xn|SP, #imm9]
      case 0b10'111'000'10'0:
        return Some(TrapMachineInsn::Load32);
      // 01 111 000 11 0 imm9 00 n t = LDURSH Wt, [Xn|SP, #imm9]
      // 01 111 000 10 0 imm9 00 n t = LDURSH Xt, [Xn|SP, #imm9]
      case 0b01'111'000'11'0:
      case 0b01'111'000'10'0:
        return Some(TrapMachineInsn::Load16);
    }
  }

  // Sign extending loads, reg + offset, scaled

  switch (INSN(31, 22)) {
    // 10 111 001 10 imm12 n t = LDRSW Xt, [Xn|SP, #imm12 * 4]
    case 0b10'111'001'10:
      return Some(TrapMachineInsn::Load32);
    // 01 111 001 10 imm12 n t = LDRSH Xt, [Xn|SP, #imm12 * 2]
    // 01 111 001 11 imm12 n t = LDRSH Wt, [Xn|SP, #imm12 * 2]
    case 0b01'111'001'10:
    case 0b01'111'001'11:
      return Some(TrapMachineInsn::Load16);
    // 00 111 001 10 imm12 n t = LDRSB Xt, [Xn|SP, #imm12 * 1]
    // 00 111 001 11 imm12 n t = LDRSB Wt, [Xn|SP, #imm12 * 1]
    case 0b00'111'001'10:
    case 0b00'111'001'11:
      return Some(TrapMachineInsn::Load8);
  }

  // Sign extending loads, reg + reg(extended/shifted)

  if (INSN(11, 10) == 0b10) {
    switch (INSN(31, 21)) {
      // 10 1110001 01 m opt s 10 n t = LDRSW Xt, [Xn|SP, R<m>{ext/sh}]
      case 0b10'1110001'01:
        return Some(TrapMachineInsn::Load32);
      // 01 1110001 01 m opt s 10 n t = LDRSH Xt, [Xn|SP, R<m>{ext/sh}]
      case 0b01'1110001'01:
        return Some(TrapMachineInsn::Load16);
      // 01 1110001 11 m opt s 10 n t = LDRSH Wt, [Xn|SP, R<m>{ext/sh}]
      case 0b01'1110001'11:
        return Some(TrapMachineInsn::Load16);
      // 00 1110001 01 m opt s 10 n t = LDRSB Xt, [Xn|SP, R<m>{ext/sh}]
      case 0b00'1110001'01:
        return Some(TrapMachineInsn::Load8);
      // 00 1110001 11 m opt s 10 n t = LDRSB Wt, [Xn|SP, R<m>{ext/sh}]
      case 0b00'1110001'11:
        return Some(TrapMachineInsn::Load8);
    }
  }

  // Plain and zero-extending loads/stores, reg + reg(extended/shifted)

  if (INSN(11, 10) == 0b10) {
    switch (INSN(31, 21)) {
      // 11 111000001 m opt s 10 n t = STR Xt, [Xn|SP, Rm{ext/sh}]
      case 0b11'111000001:
        return Some(TrapMachineInsn::Store64);
      // 10 111000001 m opt s 10 n t = STR Wt, [Xn|SP, Rm{ext/sh}]
      case 0b10'111000001:
        return Some(TrapMachineInsn::Store32);
      // 01 111000001 m opt s 10 n t = STRH Wt, [Xn|SP, Rm{ext/sh}]
      case 0b01'111000001:
        return Some(TrapMachineInsn::Store16);
      // 00 111000001 m opt s 10 n t = STRB Wt, [Xn|SP, Rm{ext/sh}]
      case 0b00'111000001:
        return Some(TrapMachineInsn::Store8);
      // 11 111000011 m opt s 10 n t = LDR Xt, [Xn|SP, Rm{ext/sh}]
      case 0b11'111000011:
        return Some(TrapMachineInsn::Load64);
      // 10 111000011 m opt s 10 n t = LDR Wt, [Xn|SP, Rm{ext/sh}]
      case 0b10'111000011:
        return Some(TrapMachineInsn::Load32);
      // 01 111000011 m opt s 10 n t = LDRH Wt, [Xn|SP, Rm{ext/sh}]
      case 0b01'111000011:
        return Some(TrapMachineInsn::Load16);
      // 00 111000011 m opt s 10 n t = LDRB Wt, [Xn|SP, Rm{ext/sh}]
      case 0b00'111000011:
        return Some(TrapMachineInsn::Load8);
    }
  }

  // SIMD - scalar FP

  switch (INSN(31, 22)) {
    // 11 111 101 00 imm12 n t = STR Dt, [Xn|SP + imm12 * 8]
    case 0b11'111'101'00:
      return Some(TrapMachineInsn::Store64);
    // 10 111 101 00 imm12 n t = STR St, [Xn|SP + imm12 * 4]
    case 0b10'111'101'00:
      return Some(TrapMachineInsn::Store32);
    // 11 111 101 01 imm12 n t = LDR Dt, [Xn|SP + imm12 * 8]
    case 0b11'111'101'01:
      return Some(TrapMachineInsn::Load64);
    // 10 111 101 01 imm12 n t = LDR St, [Xn|SP + imm12 * 4]
    case 0b10'111'101'01:
      return Some(TrapMachineInsn::Load32);
  }

  if (INSN(11, 10) == 0b00) {
    switch (INSN(31, 21)) {
      // 11 111 100 00 0 imm9 00 n t = STUR Dt, [Xn|SP, #imm9]
      case 0b11'111'100'00'0:
        return Some(TrapMachineInsn::Store64);
      // 10 111 100 00 0 imm9 00 n t = STUR St, [Xn|SP, #imm9]
      case 0b10'111'100'00'0:
        return Some(TrapMachineInsn::Store32);
      // 11 111 100 01 0 imm9 00 n t = LDUR Dt, [Xn|SP, #imm9]
      case 0b11'111'100'01'0:
        return Some(TrapMachineInsn::Load64);
      // 10 111 100 01 0 imm9 00 n t = LDUR St, [Xn|SP, #imm9]
      case 0b10'111'100'01'0:
        return Some(TrapMachineInsn::Load32);
    }
  }

  if (INSN(11, 10) == 0b10) {
    switch (INSN(31, 21)) {
      // 11 111100 001 m opt s 10 n t = STR Dt, [Xn|SP, Rm{ext/sh}]
      case 0b11'111100'001:
        return Some(TrapMachineInsn::Store64);
      // 10 111100 001 m opt s 10 n t = STR St, [Xn|SP, Rm{ext/sh}]
      case 0b10'111100'001:
        return Some(TrapMachineInsn::Store32);
      // 11 111100 011 m opt s 10 n t = LDR Dt, [Xn|SP, Rm{ext/sh}]
      case 0b11'111100'011:
        return Some(TrapMachineInsn::Load64);
      // 10 111100 011 m opt s 10 n t = LDR St, [Xn|SP, Rm{ext/sh}]
      case 0b10'111100'011:
        return Some(TrapMachineInsn::Load32);
    }
  }

  // SIMD - whole register

  if (INSN(11, 10) == 0b00) {
    // 00 111 100 10 0 imm9 00 n t = STUR Qt, [Xn|SP, #imm9]
    if (INSN(31, 21) == 0b00'111'100'10'0) {
      return Some(TrapMachineInsn::Store128);
    }
    // 00 111 100 11 0 imm9 00 n t = LDUR Qt, [Xn|SP, #imm9]
    if (INSN(31, 21) == 0b00'111'100'11'0) {
      return Some(TrapMachineInsn::Load128);
    }
  }

  // 00 111 101 10 imm12 n t = STR Qt, [Xn|SP + imm12 * 16]
  if (INSN(31, 22) == 0b00'111'101'10) {
    return Some(TrapMachineInsn::Store128);
  }
  // 00 111 101 11 imm12 n t = LDR Qt, [Xn|SP + imm12 * 16]
  if (INSN(31, 22) == 0b00'111'101'11) {
    return Some(TrapMachineInsn::Load128);
  }

  if (INSN(11, 10) == 0b10) {
    // 00 111100 101 m opt s 10 n t = STR Qt, [Xn|SP, Rm{ext/sh}]
    if (INSN(31, 21) == 0b00'111100'101) {
      return Some(TrapMachineInsn::Store128);
    }
    // 00 111100 111 m opt s 10 n t = LDR Qt, [Xn|SP, Rm{ext/sh}]
    if (INSN(31, 21) == 0b00'111100'111) {
      return Some(TrapMachineInsn::Load128);
    }
  }

  // Atomics - loads/stores "exclusive" (with reservation) (LL/SC)

  switch (INSN(31, 10)) {
    // 11 001000 010 11111 0 11111 n t = LDXR  Xt, [Xn|SP]
    case 0b11'001000'010'11111'0'11111:
      return Some(TrapMachineInsn::Load64);
    // 10 001000 010 11111 0 11111 n t = LDXR  Wt, [Xn|SP]
    case 0b10'001000'010'11111'0'11111:
      return Some(TrapMachineInsn::Load32);
    // 01 001000 010 11111 0 11111 n t = LDXRH Wt, [Xn|SP]
    case 0b01'001000'010'11111'0'11111:
      return Some(TrapMachineInsn::Load16);
    // 00 001000 010 11111 0 11111 n t = LDXRB Wt, [Xn|SP]
    case 0b00'001000'010'11111'0'11111:
      return Some(TrapMachineInsn::Load8);
      // We are never asked to examine store-exclusive instructions, because any
      // store-exclusive should be preceded by a load-exclusive instruction of
      // the same size and for the same address.  So the TrapSite is omitted for
      // the store-exclusive since the load-exclusive will trap first.
  }

  // Atomics - atomic memory operations which do (LD- variants) or do not
  // (ST-variants) return the original value at the location.

  // 11 111 0000 11 s 0 000 00 n 11111 = STADDL  Xs, [Xn|SP]
  // 10 111 0000 11 s 0 000 00 n 11111 = STADDL  Ws, [Xn|SP]
  // 01 111 0000 11 s 0 000 00 n 11111 = STADDLH Ws, [Xn|SP]
  // 00 111 0000 11 s 0 000 00 n 11111 = STADDLB Ws, [Xn|SP]
  // and the same for
  // ---------------- 0 001 00 ------- = STCLRL
  // ---------------- 0 010 00 ------- = STEORL
  // ---------------- 0 011 00 ------- = STSETL
  if (INSN(29, 21) == 0b111'0000'11 && INSN(4, 0) == 0b11111) {
    switch (INSN(15, 10)) {
      case 0b0'000'00:  // STADDL
      case 0b0'001'00:  // STCLRL
      case 0b0'010'00:  // STEORL
      case 0b0'011'00:  // STSETL
        return Some(TrapMachineInsn::Atomic);
    }
  }

  // 11 111 0001 11 s 0 000 00 n t = LDADDAL  Xs, Xt, [Xn|SP]
  // 10 111 0001 11 s 0 000 00 n t = LDADDAL  Ws, Wt, [Xn|SP]
  // 01 111 0001 11 s 0 000 00 n t = LDADDALH Ws, Wt, [Xn|SP]
  // 00 111 0001 11 s 0 000 00 n t = LDADDALB Ws, Wt, [Xn|SP]
  // and the same for
  // ---------------- 0 001 00 --- = LDCLRAL
  // ---------------- 0 010 00 --- = LDEORAL
  // ---------------- 0 011 00 --- = LDSETAL
  if (INSN(29, 21) == 0b111'0001'11) {
    switch (INSN(15, 10)) {
      case 0b0'000'00:  // LDADDAL
      case 0b0'001'00:  // LDCLRAL
      case 0b0'010'00:  // LDEORAL
      case 0b0'011'00:  // LDSETAL
        return Some(TrapMachineInsn::Atomic);
    }
  }

  // Atomics -- compare-and-swap and plain swap

  // 11 001000111 s 111111 n t = CASAL  Xs, Xt, [Xn|SP]
  // 10 001000111 s 111111 n t = CASAL  Ws, Wt, [Xn|SP]
  // 01 001000111 s 111111 n t = CASALH Ws, Wt, [Xn|SP]
  // 00 001000111 s 111111 n t = CASALB Ws, Wt, [Xn|SP]
  if (INSN(29, 21) == 0b001000111 && INSN(15, 10) == 0b111111) {
    return Some(TrapMachineInsn::Atomic);
  }

  // 11 11100011 1 s 100000 n t = SWPAL  Xs, Xt, [Xn|SP]
  // 10 11100011 1 s 100000 n t = SWPAL  Ws, Wt, [Xn|SP]
  // 01 11100011 1 s 100000 n t = SWPALH Ws, Wt, [Xn|SP]
  // 00 11100011 1 s 100000 n t = SWPALB Ws, Wt, [Xn|SP]
  if (INSN(29, 21) == 0b11100011'1 && INSN(15, 10) == 0b100000) {
    return Some(TrapMachineInsn::Atomic);
  }

#    undef INSN

  // The instruction was not identified.

  // This is useful for diagnosing decoding failures.
  // if (0) {
  //   fprintf(stderr, "insn = ");
  //   for (int i = 31; i >= 0; i--) {
  //     fprintf(stderr, "%c", ((insn >> i) & 1) ? '1' : '0');
  //     if (i < 31 && (i % 4) == 0) fprintf(stderr, " ");
  //   }
  //   fprintf(stderr, "\n");
  // }

  return Nothing();
}

// =================================================================== arm ====

#  elif defined(JS_CODEGEN_ARM)

Maybe<TrapMachineInsn> SummarizeTrapInstruction(const uint8_t* insnAddr) {
  // Almost all AArch32 instructions that use the ARM encoding (not Thumb) use
  // bits 31:28 as the guarding condition.  Since we do not expect to
  // encounter conditional loads or stores, most of the following is hardcoded
  // to check that those bits are 1110 (0xE), which is the "always execute"
  // condition.  An exception is Neon instructions, which are never
  // conditional and so have those bits set to 1111 (0xF).

  // Check instruction alignment.
  MOZ_ASSERT(0 == (uintptr_t(insnAddr) & 3));

  const uint32_t insn = *(uint32_t*)insnAddr;

#    define INSN(_maxIx, _minIx) \
      ((insn >> (_minIx)) & ((uint32_t(1) << ((_maxIx) - (_minIx) + 1)) - 1))

  // MacroAssembler::wasmTrapInstruction uses this to create SIGILL.
  if (insn == 0xE7F000F0) {
    return Some(TrapMachineInsn::OfficialUD);
  }

  // 31   27   23   19 15 11
  // cond 0101 U000 Rn Rt imm12 = STR<cond>  Rt, [Rn, +/- #imm12]
  // cond 0101 U001 Rn Rt imm12 = LDR<cond>  Rt, [Rn, +/- #imm12]
  // cond 0101 U100 Rn Rt imm12 = STRB<cond> Rt, [Rn, +/- #imm12]
  // cond 0101 U101 Rn Rt imm12 = LDRB<cond> Rt, [Rn, +/- #imm12]
  // if cond != 1111 and Rn != 1111
  // U = 1 for +, U = 0 for -
  if (INSN(31, 28) == 0b1110  // unconditional
      && INSN(27, 24) == 0b0101 && INSN(19, 16) != 0b1111) {
    switch (INSN(22, 20)) {
      case 0b000:
        return Some(TrapMachineInsn::Store32);
      case 0b001:
        return Some(TrapMachineInsn::Load32);
      case 0b100:
        return Some(TrapMachineInsn::Store8);
      case 0b101:
        return Some(TrapMachineInsn::Load8);
      default:
        break;
    }
  }

  // 31   27   23   19 15 11   7    3
  // cond 0001 U100 Rn Rt imm4 1011 imm4 = STRH<cond>  Rt, [Rn +/- #imm8]
  // cond 0001 U101 Rn Rt imm4 1101 imm4 = LDRSB<cond> Rt, [Rn +/- #imm8]
  // cond 0001 U101 Rn Rt imm4 1111 imm4 = LDRSH<cond> Rt, [Rn +/- #imm8]
  // cond 0001 U101 Rn Rt imm4 1011 imm4 = LDRH<cond>  Rt, [Rn +/- #imm8]
  // U = 1 for +, U = 0 for -
  if (INSN(31, 28) == 0b1110  // unconditional
      && INSN(27, 24) == 0b0001 && INSN(22, 21) == 0b10) {
    switch ((INSN(20, 20) << 4) | INSN(7, 4)) {
      case 0b0'1011:
        return Some(TrapMachineInsn::Store16);
      case 0b1'1101:
        return Some(TrapMachineInsn::Load8);
      case 0b1'1111:
        return Some(TrapMachineInsn::Load16);
      case 0b1'1011:
        return Some(TrapMachineInsn::Load16);
      default:
        break;
    }
  }

  // clang-format off
  //
  // 31   27   23   19 15 11    6  4 3
  // cond 0111 U000 Rn Rt shimm 00 0 Rm = STR<cond>  Rt, [Rn, +/- Rm, [lsl #shimm]]
  // cond 0111 U100 Rn Rt shimm 00 0 Rm = STRB<cond> Rt, [Rn, +/- Rm, [lsl #shimm]]
  // cond 0111 U001 Rn Rt shimm 00 0 Rm = LDR<cond>  Rt, [Rn, +/- Rm, [lsl #shimm]]
  // cond 0111 U101 Rn Rt shimm 00 0 Rm = LDRB<cond> Rt, [Rn, +/- Rm, [lsl #shimm]]
  // U = 1 for +, U = 0 for -
  //
  // clang-format on
  if (INSN(31, 28) == 0b1110                             // unconditional
      && INSN(27, 24) == 0b0111 && INSN(6, 4) == 0b00'0  // lsl
  ) {
    switch (INSN(22, 20)) {
      case 0b000:
        return Some(TrapMachineInsn::Store32);
      case 0b100:
        return Some(TrapMachineInsn::Store8);
      case 0b001:
        return Some(TrapMachineInsn::Load32);
      case 0b101:
        return Some(TrapMachineInsn::Load8);
      default:
        break;
    }
  }

  // 31   27   23   19 15 11   7    3
  // cond 0001 U000 Rn Rt 0000 1011 Rm = STRH<cond>  Rt, [Rn, +/- Rm]
  // cond 0001 U001 Rn Rt 0000 1011 Rm = LDRH<cond>  Rt, [Rn, +/- Rm]
  // cond 0001 U001 Rn Rt 0000 1101 Rm = LDRSB<cond> Rt, [Rn, +/- Rm]
  // cond 0001 U001 Rn Rt 0000 1111 Rm = LDRSH<cond> Rt, [Rn, +/- Rm]
  if (INSN(31, 28) == 0b1110  // unconditional
      && INSN(27, 24) == 0b0001 && INSN(22, 21) == 0b00 &&
      INSN(11, 8) == 0b0000) {
    switch ((INSN(20, 20) << 4) | INSN(7, 4)) {
      case 0b0'1011:
        return Some(TrapMachineInsn::Store16);
      case 0b1'1011:
        return Some(TrapMachineInsn::Load16);
      case 0b1'1101:
        return Some(TrapMachineInsn::Load8);
      case 0b1'1111:
        return Some(TrapMachineInsn::Load16);
      default:
        break;
    }
  }

  // 31   27   23   19 15 11   7
  // cond 1101 UD00 Rn Vd 1010 imm8 = VSTR<cond> Sd, [Rn +/- #imm8]
  // cond 1101 UD00 Rn Vd 1011 imm8 = VSTR<cond> Dd, [Rn +/- #imm8]
  // cond 1101 UD01 Rn Vd 1010 imm8 = VLDR<cond> Sd, [Rn +/- #imm8]
  // cond 1101 UD01 Rn Vd 1011 imm8 = VLDR<cond> Dd, [Rn +/- #imm8]
  // U = 1 for +, U = 0 for -
  // D is an extension of Vd (so can be anything)
  if (INSN(31, 28) == 0b1110  // unconditional
      && INSN(27, 24) == 0b1101 && INSN(21, 21) == 0b0) {
    switch ((INSN(20, 20) << 4) | (INSN(11, 8))) {
      case 0b0'1010:
        return Some(TrapMachineInsn::Store32);
      case 0b0'1011:
        return Some(TrapMachineInsn::Store64);
      case 0b1'1010:
        return Some(TrapMachineInsn::Load32);
      case 0b1'1011:
        return Some(TrapMachineInsn::Load64);
      default:
        break;
    }
  }

  // 31   27   23   19 15 11   7    3
  // 1111 0100 1D00 Rn Vd 1000 0000 1111 = VST1.32 {Dd[0], [Rn]
  // 1111 0100 1D10 Rn Vd 1000 0000 1111 = VLD1.32 {Dd[0], [Rn]
  if (INSN(31, 23) == 0b1111'0100'1 && INSN(20, 20) == 0 &&
      INSN(11, 0) == 0b1000'0000'1111) {
    return INSN(21, 21) == 1 ? Some(TrapMachineInsn::Load32)
                             : Some(TrapMachineInsn::Store32);
  }

  // 31   27   23   19 15 11   7    3
  // 1111 0100 0D00 Rn Vd 0111 1100 1111 = VST1.64 {Dd], [Rn]
  // 1111 0100 0D10 Rn Vd 0111 1100 1111 = VLD1.64 {Dd], [Rn]
  if (INSN(31, 23) == 0b1111'0100'0 && INSN(20, 20) == 0 &&
      INSN(11, 0) == 0b0111'1100'1111) {
    return INSN(21, 21) == 1 ? Some(TrapMachineInsn::Load64)
                             : Some(TrapMachineInsn::Store64);
  }

  // 31   27   23   19 15 11   7    3
  // cond 0001 1101 n  t  1111 1001 1111 = LDREXB<cond> Rt, [Rn]
  // cond 0001 1111 n  t  1111 1001 1111 = LDREXH<cond> Rt, [Rn]
  // cond 0001 1001 n  t  1111 1001 1111 = LDREX<cond>  Rt, [Rn]
  // cond 0001 1011 n  t  1111 1001 1111 = LDREXD<cond> Rt, [Rn]
  if (INSN(31, 23) == 0b1110'0001'1 && INSN(11, 0) == 0b1111'1001'1111) {
    switch (INSN(22, 20)) {
      case 0b101:
        return Some(TrapMachineInsn::Load8);
      case 0b111:
        return Some(TrapMachineInsn::Load16);
      case 0b001:
        return Some(TrapMachineInsn::Load32);
      case 0b011:
        return Some(TrapMachineInsn::Load64);
      default:
        break;
    }
  }

#    undef INSN

  // The instruction was not identified.

  // This is useful for diagnosing decoding failures.
  // if (0) {
  //   fprintf(stderr, "insn = ");
  //   for (int i = 31; i >= 0; i--) {
  //     fprintf(stderr, "%c", ((insn >> i) & 1) ? '1' : '0');
  //     if (i < 31 && (i % 4) == 0) fprintf(stderr, " ");
  //   }
  //   fprintf(stderr, "\n");
  // }

  return Nothing();
}

// =============================================================== riscv64 ====

#  elif defined(JS_CODEGEN_RISCV64)

Maybe<TrapMachineInsn> SummarizeTrapInstruction(const uint8_t* insnAddr) {
  // Check instruction alignment.
  MOZ_ASSERT(0 == (uintptr_t(insnAddr) & 3));

  const uint32_t insn = *(uint32_t*)insnAddr;

#    define INSN(_maxIx, _minIx) \
      ((insn >> (_minIx)) & ((uint32_t(1) << ((_maxIx) - (_minIx) + 1)) - 1))
  // MacroAssembler::wasmTrapInstruction uses this to create SIGILL.
  if (insn ==
      (RO_CSRRWI | csr_cycle << kCsrShift | kWasmTrapCode << kRs1Shift)) {
    return Some(TrapMachineInsn::OfficialUD);
  }

  if (INSN(6, 0) == STORE) {
    switch (INSN(14, 12)) {
      case 0b011:
        return Some(TrapMachineInsn::Load64);
      case 0b010:
        return Some(TrapMachineInsn::Load32);
      case 0b001:
        return Some(TrapMachineInsn::Load16);
      case 0b000:
        return Some(TrapMachineInsn::Load8);
      default:
        break;
    }
  }

  if (INSN(6, 0) == LOAD) {
    switch (INSN(14, 12)) {
      case 0b011:
        return Some(TrapMachineInsn::Store64);
      case 0b010:
        return Some(TrapMachineInsn::Store32);
      case 0b001:
        return Some(TrapMachineInsn::Store16);
      case 0b000:
        return Some(TrapMachineInsn::Store8);
      default:
        break;
    }
  }

  if (INSN(6, 0) == LOAD_FP) {
    switch (INSN(14, 12)) {
      case 0b011:
        return Some(TrapMachineInsn::Load64);
      case 0b010:
        return Some(TrapMachineInsn::Load32);
      default:
        break;
    }
  }

  if (INSN(6, 0) == STORE_FP) {
    switch (INSN(14, 12)) {
      case 0b011:
        return Some(TrapMachineInsn::Store64);
      case 0b010:
        return Some(TrapMachineInsn::Store32);
      default:
        break;
    }
  }

  if (INSN(6, 0) == AMO && INSN(31, 27) == 00010) {
    switch (INSN(14, 12)) {
      case 0b011:
        return Some(TrapMachineInsn::Load64);
      case 0b010:
        return Some(TrapMachineInsn::Load32);
      default:
        break;
    }
  }

  if (INSN(6, 0) == AMO && INSN(31, 27) == 00011) {
    switch (INSN(14, 12)) {
      case 0b011:
        return Some(TrapMachineInsn::Store64);
      case 0b010:
        return Some(TrapMachineInsn::Store32);
      default:
        break;
    }
  }

#    undef INSN

  return Nothing();
}

// =========================================================== loongarch64 ====

#  elif defined(JS_CODEGEN_LOONG64)

Maybe<TrapMachineInsn> SummarizeTrapInstruction(const uint8_t* insnAddr) {
  // Check instruction alignment.
  MOZ_ASSERT(0 == (uintptr_t(insnAddr) & 3));

  const uint32_t insn = *(uint32_t*)insnAddr;

#    define INSN(_maxIx, _minIx) \
      ((insn >> (_minIx)) & ((uint32_t(1) << ((_maxIx) - (_minIx) + 1)) - 1))

  // LoongArch instructions encoding document:
  // https://loongson.github.io/LoongArch-Documentation/LoongArch-Vol1-EN#table-of-instruction-encoding

  // MacroAssembler::wasmTrapInstruction uses this to create SIGILL.
  // break 0x6
  if (insn == 0x002A0006) {
    return Some(TrapMachineInsn::OfficialUD);
  }

  // Loads/stores with reg + offset (si12).
  if (INSN(31, 26) == 0b001010) {
    switch (INSN(25, 22)) {
      // ld.b  rd, rj, si12
      case 0b0000:
        return Some(TrapMachineInsn::Load8);
      // ld.h  rd, rj, si12
      case 0b0001:
        return Some(TrapMachineInsn::Load16);
      // ld.w  rd, rj, si12
      case 0b0010:
        return Some(TrapMachineInsn::Load32);
      // ld.d  rd, rj, si12
      case 0b0011:
        return Some(TrapMachineInsn::Load64);
      // st.b  rd, rj, si12
      case 0b0100:
        return Some(TrapMachineInsn::Store8);
      // st.h  rd, rj, si12
      case 0b0101:
        return Some(TrapMachineInsn::Store16);
      // st.w  rd, rj, si12
      case 0b0110:
        return Some(TrapMachineInsn::Store32);
      // st.d  rd, rj, si12
      case 0b0111:
        return Some(TrapMachineInsn::Store64);
      // ld.bu  rd, rj, si12
      case 0b1000:
        return Some(TrapMachineInsn::Load8);
      // ld.hu  rd, rj, si12
      case 0b1001:
        return Some(TrapMachineInsn::Load16);
      // ld.wu  rd, rj, si12
      case 0b1010:
        return Some(TrapMachineInsn::Load32);
      // preld  hint, rj, si12
      case 0b1011:
        break;
      // fld.s  fd, rj, si12
      case 0b1100:
        return Some(TrapMachineInsn::Load32);
      // fst.s  fd, rj, si12
      case 0b1101:
        return Some(TrapMachineInsn::Store32);
      // fld.d  fd, rj, si12
      case 0b1110:
        return Some(TrapMachineInsn::Load64);
      // fst.s  fd, rj, si12
      case 0b1111:
        return Some(TrapMachineInsn::Store64);
      default:
        break;
    }
  }

  // Loads/stores with reg + reg.
  if (INSN(31, 22) == 0b0011100000 && INSN(17, 15) == 0b000) {
    switch (INSN(21, 18)) {
      // ldx.b  rd, rj, rk
      case 0b0000:
        return Some(TrapMachineInsn::Load8);
      // ldx.h  rd, rj, rk
      case 0b0001:
        return Some(TrapMachineInsn::Load16);
      // ldx.w  rd, rj, rk
      case 0b0010:
        return Some(TrapMachineInsn::Load32);
      // ldx.d  rd, rj, rk
      case 0b0011:
        return Some(TrapMachineInsn::Load64);
      // stx.b  rd, rj, rk
      case 0b0100:
        return Some(TrapMachineInsn::Store8);
      // stx.h  rd, rj, rk
      case 0b0101:
        return Some(TrapMachineInsn::Store16);
      // stx.w  rd, rj, rk
      case 0b0110:
        return Some(TrapMachineInsn::Store32);
      // stx.d  rd, rj, rk
      case 0b0111:
        return Some(TrapMachineInsn::Store64);
      // ldx.bu  rd, rj, rk
      case 0b1000:
        return Some(TrapMachineInsn::Load8);
      // ldx.hu  rd, rj, rk
      case 0b1001:
        return Some(TrapMachineInsn::Load16);
      // ldx.wu  rd, rj, rk
      case 0b1010:
        return Some(TrapMachineInsn::Load32);
      // preldx  hint, rj, rk
      case 0b1011:
        break;
      // fldx.s  fd, rj, rk
      case 0b1100:
        return Some(TrapMachineInsn::Load32);
      // fldx.d  fd, rj, rk
      case 0b1101:
        return Some(TrapMachineInsn::Load64);
      // fstx.s  fd, rj, rk
      case 0b1110:
        return Some(TrapMachineInsn::Store32);
      // fstx.d  fd, rj, rk
      case 0b1111:
        return Some(TrapMachineInsn::Store64);
      default:
        break;
    }
  }

  // Loads/stores with reg + offset (si14).
  //   1. Atomics - loads/stores "exclusive" (with reservation) (LL/SC)
  //   2. {ld/st}ptr.{w/d}
  if (INSN(31, 27) == 0b00100) {
    switch (INSN(26, 24)) {
      // ll.w  rd, rj, si14
      case 0b000:
        return Some(TrapMachineInsn::Load32);
      // ll.d  rd, rj, si14
      case 0b010:
        return Some(TrapMachineInsn::Load64);
      // ldptr.w  rd, rj, si14
      case 0b100:
        return Some(TrapMachineInsn::Load32);
      // stptr.w  rd, rj, si14
      case 0b101:
        return Some(TrapMachineInsn::Store32);
      // ldptr.d  rd, rj, si14
      case 0b110:
        return Some(TrapMachineInsn::Load64);
      // stptr.d  rd, rj, si14
      case 0b111:
        return Some(TrapMachineInsn::Store64);
      default:
        break;
        // We are never asked to examine store-exclusive instructions, because
        // any store-exclusive should be preceded by a load-exclusive
        // instruction of the same size and for the same address.  So the
        // TrapSite is omitted for the store-exclusive since the load-exclusive
        // will trap first.
    }
  }

#    undef INSN

  return Nothing();
}

// ================================================================== none ====

#  elif defined(JS_CODEGEN_NONE)

Maybe<TrapMachineInsn> SummarizeTrapInstruction(const uint8_t* insnAddr) {
  MOZ_CRASH();
}

// ================================================================= other ====

#  else

#    error "SummarizeTrapInstruction: not implemented on this architecture"

#  endif  // defined(JS_CODEGEN_*)

#endif  // defined(DEBUG)

}  // namespace wasm
}  // namespace js
