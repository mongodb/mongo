// Copyright 2014, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "jit/arm64/vixl/Decoder-vixl.h"

#include <algorithm>

#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"

namespace vixl {

void Decoder::DecodeInstruction(const Instruction *instr) {
  if (instr->Bits(28, 27) == 0) {
    VisitUnallocated(instr);
  } else {
    switch (instr->Bits(27, 24)) {
      // 0:   PC relative addressing.
      case 0x0: DecodePCRelAddressing(instr); break;

      // 1:   Add/sub immediate.
      case 0x1: DecodeAddSubImmediate(instr); break;

      // A:   Logical shifted register.
      //      Add/sub with carry.
      //      Conditional compare register.
      //      Conditional compare immediate.
      //      Conditional select.
      //      Data processing 1 source.
      //      Data processing 2 source.
      // B:   Add/sub shifted register.
      //      Add/sub extended register.
      //      Data processing 3 source.
      case 0xA:
      case 0xB: DecodeDataProcessing(instr); break;

      // 2:   Logical immediate.
      //      Move wide immediate.
      case 0x2: DecodeLogical(instr); break;

      // 3:   Bitfield.
      //      Extract.
      case 0x3: DecodeBitfieldExtract(instr); break;

      // 4:   Unconditional branch immediate.
      //      Exception generation.
      //      Compare and branch immediate.
      // 5:   Compare and branch immediate.
      //      Conditional branch.
      //      System.
      // 6,7: Unconditional branch.
      //      Test and branch immediate.
      case 0x4:
      case 0x5:
      case 0x6:
      case 0x7: DecodeBranchSystemException(instr); break;

      // 8,9: Load/store register pair post-index.
      //      Load register literal.
      //      Load/store register unscaled immediate.
      //      Load/store register immediate post-index.
      //      Load/store register immediate pre-index.
      //      Load/store register offset.
      //      Load/store exclusive.
      // C,D: Load/store register pair offset.
      //      Load/store register pair pre-index.
      //      Load/store register unsigned immediate.
      //      Advanced SIMD.
      case 0x8:
      case 0x9:
      case 0xC:
      case 0xD: DecodeLoadStore(instr); break;

      // E:   FP fixed point conversion.
      //      FP integer conversion.
      //      FP data processing 1 source.
      //      FP compare.
      //      FP immediate.
      //      FP data processing 2 source.
      //      FP conditional compare.
      //      FP conditional select.
      //      Advanced SIMD.
      // F:   FP data processing 3 source.
      //      Advanced SIMD.
      case 0xE:
      case 0xF: DecodeFP(instr); break;
    }
  }
}

void Decoder::AppendVisitor(DecoderVisitor* new_visitor) {
  MOZ_ALWAYS_TRUE(visitors_.append(new_visitor));
}


void Decoder::PrependVisitor(DecoderVisitor* new_visitor) {
  MOZ_ALWAYS_TRUE(visitors_.insert(visitors_.begin(), new_visitor));
}


void Decoder::InsertVisitorBefore(DecoderVisitor* new_visitor,
                                  DecoderVisitor* registered_visitor) {
  for (auto it = visitors_.begin(); it != visitors_.end(); it++) {
    if (*it == registered_visitor) {
      MOZ_ALWAYS_TRUE(visitors_.insert(it, new_visitor));
      return;
    }
  }
  // We reached the end of the list without finding registered_visitor.
  MOZ_ALWAYS_TRUE(visitors_.append(new_visitor));
}


void Decoder::InsertVisitorAfter(DecoderVisitor* new_visitor,
                                 DecoderVisitor* registered_visitor) {
  for (auto it = visitors_.begin(); it != visitors_.end(); it++) {
    if (*it == registered_visitor) {
      it++;
      MOZ_ALWAYS_TRUE(visitors_.insert(it, new_visitor));
      return;
    }
  }
  // We reached the end of the list without finding registered_visitor.
  MOZ_ALWAYS_TRUE(visitors_.append(new_visitor));
}


void Decoder::RemoveVisitor(DecoderVisitor* visitor) {
  visitors_.erase(std::remove(visitors_.begin(), visitors_.end(), visitor),
                  visitors_.end());
}


void Decoder::DecodePCRelAddressing(const Instruction* instr) {
  VIXL_ASSERT(instr->Bits(27, 24) == 0x0);
  // We know bit 28 is set, as <b28:b27> = 0 is filtered out at the top level
  // decode.
  VIXL_ASSERT(instr->Bit(28) == 0x1);
  VisitPCRelAddressing(instr);
}


void Decoder::DecodeBranchSystemException(const Instruction* instr) {
  VIXL_ASSERT((instr->Bits(27, 24) == 0x4) ||
              (instr->Bits(27, 24) == 0x5) ||
              (instr->Bits(27, 24) == 0x6) ||
              (instr->Bits(27, 24) == 0x7) );

  switch (instr->Bits(31, 29)) {
    case 0:
    case 4: {
      VisitUnconditionalBranch(instr);
      break;
    }
    case 1:
    case 5: {
      if (instr->Bit(25) == 0) {
        VisitCompareBranch(instr);
      } else {
        VisitTestBranch(instr);
      }
      break;
    }
    case 2: {
      if (instr->Bit(25) == 0) {
        if ((instr->Bit(24) == 0x1) ||
            (instr->Mask(0x01000010) == 0x00000010)) {
          VisitUnallocated(instr);
        } else {
          VisitConditionalBranch(instr);
        }
      } else {
        VisitUnallocated(instr);
      }
      break;
    }
    case 6: {
      if (instr->Bit(25) == 0) {
        if (instr->Bit(24) == 0) {
          if ((instr->Bits(4, 2) != 0) ||
              (instr->Mask(0x00E0001D) == 0x00200001) ||
              (instr->Mask(0x00E0001D) == 0x00400001) ||
              (instr->Mask(0x00E0001E) == 0x00200002) ||
              (instr->Mask(0x00E0001E) == 0x00400002) ||
              (instr->Mask(0x00E0001C) == 0x00600000) ||
              (instr->Mask(0x00E0001C) == 0x00800000) ||
              (instr->Mask(0x00E0001F) == 0x00A00000) ||
              (instr->Mask(0x00C0001C) == 0x00C00000)) {
            if (instr->InstructionBits() == UNDEFINED_INST_PATTERN) {
                VisitException(instr);
            } else {
                VisitUnallocated(instr);
            }
          } else {
            VisitException(instr);
          }
        } else {
          if (instr->Bits(23, 22) == 0) {
            const Instr masked_003FF0E0 = instr->Mask(0x003FF0E0);
            if ((instr->Bits(21, 19) == 0x4) ||
                (masked_003FF0E0 == 0x00033000) ||
                (masked_003FF0E0 == 0x003FF020) ||
                (masked_003FF0E0 == 0x003FF060) ||
                (masked_003FF0E0 == 0x003FF0E0) ||
                (instr->Mask(0x00388000) == 0x00008000) ||
                (instr->Mask(0x0038E000) == 0x00000000) ||
                (instr->Mask(0x0039E000) == 0x00002000) ||
                (instr->Mask(0x003AE000) == 0x00002000) ||
                (instr->Mask(0x003CE000) == 0x00042000) ||
                (instr->Mask(0x003FFFC0) == 0x000320C0) ||
                (instr->Mask(0x003FF100) == 0x00032100) ||
                // (instr->Mask(0x003FF200) == 0x00032200) || // match CSDB
                (instr->Mask(0x003FF400) == 0x00032400) ||
                (instr->Mask(0x003FF800) == 0x00032800) ||
                (instr->Mask(0x0038F000) == 0x00005000) ||
                (instr->Mask(0x0038E000) == 0x00006000)) {
              VisitUnallocated(instr);
            } else {
              VisitSystem(instr);
            }
          } else {
            VisitUnallocated(instr);
          }
        }
      } else {
        if ((instr->Bit(24) == 0x1) ||
            (instr->Bits(20, 16) != 0x1F) ||
            (instr->Bits(15, 10) != 0) ||
            (instr->Bits(4, 0) != 0) ||
            (instr->Bits(24, 21) == 0x3) ||
            (instr->Bits(24, 22) == 0x3)) {
          VisitUnallocated(instr);
        } else {
          VisitUnconditionalBranchToRegister(instr);
        }
      }
      break;
    }
    case 3:
    case 7: {
      VisitUnallocated(instr);
      break;
    }
  }
}


void Decoder::DecodeLoadStore(const Instruction* instr) {
  VIXL_ASSERT((instr->Bits(27, 24) == 0x8) ||
              (instr->Bits(27, 24) == 0x9) ||
              (instr->Bits(27, 24) == 0xC) ||
              (instr->Bits(27, 24) == 0xD) );
  // TODO(all): rearrange the tree to integrate this branch.
  if ((instr->Bit(28) == 0) && (instr->Bit(29) == 0) && (instr->Bit(26) == 1)) {
    DecodeNEONLoadStore(instr);
    return;
  }

  if (instr->Bit(24) == 0) {
    if (instr->Bit(28) == 0) {
      if (instr->Bit(29) == 0) {
        if (instr->Bit(26) == 0) {
          VisitLoadStoreExclusive(instr);
        } else {
          VIXL_UNREACHABLE();
        }
      } else {
        if ((instr->Bits(31, 30) == 0x3) ||
            (instr->Mask(0xC4400000) == 0x40000000)) {
          VisitUnallocated(instr);
        } else {
          if (instr->Bit(23) == 0) {
            if (instr->Mask(0xC4400000) == 0xC0400000) {
              VisitUnallocated(instr);
            } else {
              VisitLoadStorePairNonTemporal(instr);
            }
          } else {
            VisitLoadStorePairPostIndex(instr);
          }
        }
      }
    } else {
      if (instr->Bit(29) == 0) {
        if (instr->Mask(0xC4000000) == 0xC4000000) {
          VisitUnallocated(instr);
        } else {
          VisitLoadLiteral(instr);
        }
      } else {
        if ((instr->Mask(0x84C00000) == 0x80C00000) ||
            (instr->Mask(0x44800000) == 0x44800000) ||
            (instr->Mask(0x84800000) == 0x84800000)) {
          VisitUnallocated(instr);
        } else {
          if (instr->Bit(21) == 0) {
            switch (instr->Bits(11, 10)) {
              case 0: {
                VisitLoadStoreUnscaledOffset(instr);
                break;
              }
              case 1: {
                if (instr->Mask(0xC4C00000) == 0xC0800000) {
                  VisitUnallocated(instr);
                } else {
                  VisitLoadStorePostIndex(instr);
                }
                break;
              }
              case 2: {
                // TODO: VisitLoadStoreRegisterOffsetUnpriv.
                VisitUnimplemented(instr);
                break;
              }
              case 3: {
                if (instr->Mask(0xC4C00000) == 0xC0800000) {
                  VisitUnallocated(instr);
                } else {
                  VisitLoadStorePreIndex(instr);
                }
                break;
              }
            }
          } else {
            if (instr->Bits(11, 10) == 0x2) {
              if (instr->Bit(14) == 0) {
                VisitUnallocated(instr);
              } else {
                VisitLoadStoreRegisterOffset(instr);
              }
            } else {
              VisitUnallocated(instr);
            }
          }
        }
      }
    }
  } else {
    if (instr->Bit(28) == 0) {
      if (instr->Bit(29) == 0) {
        VisitUnallocated(instr);
      } else {
        if ((instr->Bits(31, 30) == 0x3) ||
            (instr->Mask(0xC4400000) == 0x40000000)) {
          VisitUnallocated(instr);
        } else {
          if (instr->Bit(23) == 0) {
            VisitLoadStorePairOffset(instr);
          } else {
            VisitLoadStorePairPreIndex(instr);
          }
        }
      }
    } else {
      if (instr->Bit(29) == 0) {
        VisitUnallocated(instr);
      } else {
        if ((instr->Mask(0x84C00000) == 0x80C00000) ||
            (instr->Mask(0x44800000) == 0x44800000) ||
            (instr->Mask(0x84800000) == 0x84800000)) {
          VisitUnallocated(instr);
        } else {
          VisitLoadStoreUnsignedOffset(instr);
        }
      }
    }
  }
}


void Decoder::DecodeLogical(const Instruction* instr) {
  VIXL_ASSERT(instr->Bits(27, 24) == 0x2);

  if (instr->Mask(0x80400000) == 0x00400000) {
    VisitUnallocated(instr);
  } else {
    if (instr->Bit(23) == 0) {
      VisitLogicalImmediate(instr);
    } else {
      if (instr->Bits(30, 29) == 0x1) {
        VisitUnallocated(instr);
      } else {
        VisitMoveWideImmediate(instr);
      }
    }
  }
}


void Decoder::DecodeBitfieldExtract(const Instruction* instr) {
  VIXL_ASSERT(instr->Bits(27, 24) == 0x3);

  if ((instr->Mask(0x80400000) == 0x80000000) ||
      (instr->Mask(0x80400000) == 0x00400000) ||
      (instr->Mask(0x80008000) == 0x00008000)) {
    VisitUnallocated(instr);
  } else if (instr->Bit(23) == 0) {
    if ((instr->Mask(0x80200000) == 0x00200000) ||
        (instr->Mask(0x60000000) == 0x60000000)) {
      VisitUnallocated(instr);
    } else {
      VisitBitfield(instr);
    }
  } else {
    if ((instr->Mask(0x60200000) == 0x00200000) ||
        (instr->Mask(0x60000000) != 0x00000000)) {
      VisitUnallocated(instr);
    } else {
      VisitExtract(instr);
    }
  }
}


void Decoder::DecodeAddSubImmediate(const Instruction* instr) {
  VIXL_ASSERT(instr->Bits(27, 24) == 0x1);
  if (instr->Bit(23) == 1) {
    VisitUnallocated(instr);
  } else {
    VisitAddSubImmediate(instr);
  }
}


void Decoder::DecodeDataProcessing(const Instruction* instr) {
  VIXL_ASSERT((instr->Bits(27, 24) == 0xA) ||
              (instr->Bits(27, 24) == 0xB));

  if (instr->Bit(24) == 0) {
    if (instr->Bit(28) == 0) {
      if (instr->Mask(0x80008000) == 0x00008000) {
        VisitUnallocated(instr);
      } else {
        VisitLogicalShifted(instr);
      }
    } else {
      switch (instr->Bits(23, 21)) {
        case 0: {
          if (instr->Mask(0x0000FC00) != 0) {
            VisitUnallocated(instr);
          } else {
            VisitAddSubWithCarry(instr);
          }
          break;
        }
        case 2: {
          if ((instr->Bit(29) == 0) ||
              (instr->Mask(0x00000410) != 0)) {
            VisitUnallocated(instr);
          } else {
            if (instr->Bit(11) == 0) {
              VisitConditionalCompareRegister(instr);
            } else {
              VisitConditionalCompareImmediate(instr);
            }
          }
          break;
        }
        case 4: {
          if (instr->Mask(0x20000800) != 0x00000000) {
            VisitUnallocated(instr);
          } else {
            VisitConditionalSelect(instr);
          }
          break;
        }
        case 6: {
          if (instr->Bit(29) == 0x1) {
            VisitUnallocated(instr);
            VIXL_FALLTHROUGH();
          } else {
            if (instr->Bit(30) == 0) {
              if ((instr->Bit(15) == 0x1) ||
                  (instr->Bits(15, 11) == 0) ||
                  (instr->Bits(15, 12) == 0x1) ||
                  (instr->Bits(15, 12) == 0x3) ||
                  (instr->Bits(15, 13) == 0x3) ||
                  (instr->Mask(0x8000EC00) == 0x00004C00) ||
                  (instr->Mask(0x8000E800) == 0x80004000) ||
                  (instr->Mask(0x8000E400) == 0x80004000)) {
                VisitUnallocated(instr);
              } else {
                VisitDataProcessing2Source(instr);
              }
            } else {
              if ((instr->Bit(13) == 1) ||
                  (instr->Bits(20, 16) != 0) ||
                  (instr->Bits(15, 14) != 0) ||
                  (instr->Mask(0xA01FFC00) == 0x00000C00) ||
                  (instr->Mask(0x201FF800) == 0x00001800)) {
                VisitUnallocated(instr);
              } else {
                VisitDataProcessing1Source(instr);
              }
            }
            break;
          }
        }
        case 1:
        case 3:
        case 5:
        case 7: VisitUnallocated(instr); break;
      }
    }
  } else {
    if (instr->Bit(28) == 0) {
     if (instr->Bit(21) == 0) {
        if ((instr->Bits(23, 22) == 0x3) ||
            (instr->Mask(0x80008000) == 0x00008000)) {
          VisitUnallocated(instr);
        } else {
          VisitAddSubShifted(instr);
        }
      } else {
        if ((instr->Mask(0x00C00000) != 0x00000000) ||
            (instr->Mask(0x00001400) == 0x00001400) ||
            (instr->Mask(0x00001800) == 0x00001800)) {
          VisitUnallocated(instr);
        } else {
          VisitAddSubExtended(instr);
        }
      }
    } else {
      if ((instr->Bit(30) == 0x1) ||
          (instr->Bits(30, 29) == 0x1) ||
          (instr->Mask(0xE0600000) == 0x00200000) ||
          (instr->Mask(0xE0608000) == 0x00400000) ||
          (instr->Mask(0x60608000) == 0x00408000) ||
          (instr->Mask(0x60E00000) == 0x00E00000) ||
          (instr->Mask(0x60E00000) == 0x00800000) ||
          (instr->Mask(0x60E00000) == 0x00600000)) {
        VisitUnallocated(instr);
      } else {
        VisitDataProcessing3Source(instr);
      }
    }
  }
}


void Decoder::DecodeFP(const Instruction* instr) {
  VIXL_ASSERT((instr->Bits(27, 24) == 0xE) ||
              (instr->Bits(27, 24) == 0xF));
  if (instr->Bit(28) == 0) {
    DecodeNEONVectorDataProcessing(instr);
  } else {
    if (instr->Bits(31, 30) == 0x3) {
      VisitUnallocated(instr);
    } else if (instr->Bits(31, 30) == 0x1) {
      DecodeNEONScalarDataProcessing(instr);
    } else {
      if (instr->Bit(29) == 0) {
        if (instr->Bit(24) == 0) {
          if (instr->Bit(21) == 0) {
            if ((instr->Bit(23) == 1) ||
                (instr->Bit(18) == 1) ||
                (instr->Mask(0x80008000) == 0x00000000) ||
                (instr->Mask(0x000E0000) == 0x00000000) ||
                (instr->Mask(0x000E0000) == 0x000A0000) ||
                (instr->Mask(0x00160000) == 0x00000000) ||
                (instr->Mask(0x00160000) == 0x00120000)) {
              VisitUnallocated(instr);
            } else {
              VisitFPFixedPointConvert(instr);
            }
          } else {
            if (instr->Bits(15, 10) == 32) {
              VisitUnallocated(instr);
            } else if (instr->Bits(15, 10) == 0) {
              if ((instr->Bits(23, 22) == 0x3) ||
                  (instr->Mask(0x000E0000) == 0x000A0000) ||
                  (instr->Mask(0x000E0000) == 0x000C0000) ||
                  (instr->Mask(0x00160000) == 0x00120000) ||
                  (instr->Mask(0x00160000) == 0x00140000) ||
                  (instr->Mask(0x20C40000) == 0x00800000) ||
                  (instr->Mask(0x20C60000) == 0x00840000) ||
                  (instr->Mask(0xA0C60000) == 0x80060000) ||
                  (instr->Mask(0xA0C60000) == 0x00860000) ||
                  (instr->Mask(0xA0C60000) == 0x00460000) ||
                  (instr->Mask(0xA0CE0000) == 0x80860000) ||
                  (instr->Mask(0xA0CE0000) == 0x804E0000) ||
                  (instr->Mask(0xA0CE0000) == 0x000E0000) ||
                  (instr->Mask(0xA0D60000) == 0x00160000) ||
                  (instr->Mask(0xA0D60000) == 0x80560000) ||
                  (instr->Mask(0xA0D60000) == 0x80960000)) {
                VisitUnallocated(instr);
              } else {
                VisitFPIntegerConvert(instr);
              }
            } else if (instr->Bits(14, 10) == 16) {
              const Instr masked_A0DF8000 = instr->Mask(0xA0DF8000);
              if ((instr->Mask(0x80180000) != 0) ||
                  (masked_A0DF8000 == 0x00020000) ||
                  (masked_A0DF8000 == 0x00030000) ||
                  (masked_A0DF8000 == 0x00068000) ||
                  (masked_A0DF8000 == 0x00428000) ||
                  (masked_A0DF8000 == 0x00430000) ||
                  (masked_A0DF8000 == 0x00468000) ||
                  (instr->Mask(0xA0D80000) == 0x00800000) ||
                  (instr->Mask(0xA0DE0000) == 0x00C00000) ||
                  (instr->Mask(0xA0DF0000) == 0x00C30000) ||
                  (instr->Mask(0xA0DC0000) == 0x00C40000)) {
                VisitUnallocated(instr);
              } else {
                VisitFPDataProcessing1Source(instr);
              }
            } else if (instr->Bits(13, 10) == 8) {
              if ((instr->Bits(15, 14) != 0) ||
                  (instr->Bits(2, 0) != 0) ||
                  (instr->Mask(0x80800000) != 0x00000000)) {
                VisitUnallocated(instr);
              } else {
                VisitFPCompare(instr);
              }
            } else if (instr->Bits(12, 10) == 4) {
              if ((instr->Bits(9, 5) != 0) ||
                  (instr->Mask(0x80800000) != 0x00000000)) {
                VisitUnallocated(instr);
              } else {
                VisitFPImmediate(instr);
              }
            } else {
              if (instr->Mask(0x80800000) != 0x00000000) {
                VisitUnallocated(instr);
              } else {
                switch (instr->Bits(11, 10)) {
                  case 1: {
                    VisitFPConditionalCompare(instr);
                    break;
                  }
                  case 2: {
                    if ((instr->Bits(15, 14) == 0x3) ||
                        (instr->Mask(0x00009000) == 0x00009000) ||
                        (instr->Mask(0x0000A000) == 0x0000A000)) {
                      VisitUnallocated(instr);
                    } else {
                      VisitFPDataProcessing2Source(instr);
                    }
                    break;
                  }
                  case 3: {
                    VisitFPConditionalSelect(instr);
                    break;
                  }
                  default: VIXL_UNREACHABLE();
                }
              }
            }
          }
        } else {
          // Bit 30 == 1 has been handled earlier.
          VIXL_ASSERT(instr->Bit(30) == 0);
          if (instr->Mask(0xA0800000) != 0) {
            VisitUnallocated(instr);
          } else {
            VisitFPDataProcessing3Source(instr);
          }
        }
      } else {
        VisitUnallocated(instr);
      }
    }
  }
}


void Decoder::DecodeNEONLoadStore(const Instruction* instr) {
  VIXL_ASSERT(instr->Bits(29, 25) == 0x6);
  if (instr->Bit(31) == 0) {
    if ((instr->Bit(24) == 0) && (instr->Bit(21) == 1)) {
      VisitUnallocated(instr);
      return;
    }

    if (instr->Bit(23) == 0) {
      if (instr->Bits(20, 16) == 0) {
        if (instr->Bit(24) == 0) {
          VisitNEONLoadStoreMultiStruct(instr);
        } else {
          VisitNEONLoadStoreSingleStruct(instr);
        }
      } else {
        VisitUnallocated(instr);
      }
    } else {
      if (instr->Bit(24) == 0) {
        VisitNEONLoadStoreMultiStructPostIndex(instr);
      } else {
        VisitNEONLoadStoreSingleStructPostIndex(instr);
      }
    }
  } else {
    VisitUnallocated(instr);
  }
}


void Decoder::DecodeNEONVectorDataProcessing(const Instruction* instr) {
  VIXL_ASSERT(instr->Bits(28, 25) == 0x7);
  if (instr->Bit(31) == 0) {
    if (instr->Bit(24) == 0) {
      if (instr->Bit(21) == 0) {
        if (instr->Bit(15) == 0) {
          if (instr->Bit(10) == 0) {
            if (instr->Bit(29) == 0) {
              if (instr->Bit(11) == 0) {
                VisitNEONTable(instr);
              } else {
                VisitNEONPerm(instr);
              }
            } else {
              VisitNEONExtract(instr);
            }
          } else {
            if (instr->Bits(23, 22) == 0) {
              VisitNEONCopy(instr);
            } else {
              VisitUnallocated(instr);
            }
          }
        } else {
          VisitUnallocated(instr);
        }
      } else {
        if (instr->Bit(10) == 0) {
          if (instr->Bit(11) == 0) {
            VisitNEON3Different(instr);
          } else {
            if (instr->Bits(18, 17) == 0) {
              if (instr->Bit(20) == 0) {
                if (instr->Bit(19) == 0) {
                  VisitNEON2RegMisc(instr);
                } else {
                  if (instr->Bits(30, 29) == 0x2) {
                    VisitCryptoAES(instr);
                  } else {
                    VisitUnallocated(instr);
                  }
                }
              } else {
                if (instr->Bit(19) == 0) {
                  VisitNEONAcrossLanes(instr);
                } else {
                  VisitUnallocated(instr);
                }
              }
            } else {
              VisitUnallocated(instr);
            }
          }
        } else {
          VisitNEON3Same(instr);
        }
      }
    } else {
      if (instr->Bit(10) == 0) {
        VisitNEONByIndexedElement(instr);
      } else {
        if (instr->Bit(23) == 0) {
          if (instr->Bits(22, 19) == 0) {
            VisitNEONModifiedImmediate(instr);
          } else {
            VisitNEONShiftImmediate(instr);
          }
        } else {
          VisitUnallocated(instr);
        }
      }
    }
  } else {
    VisitUnallocated(instr);
  }
}


void Decoder::DecodeNEONScalarDataProcessing(const Instruction* instr) {
  VIXL_ASSERT(instr->Bits(28, 25) == 0xF);
  if (instr->Bit(24) == 0) {
    if (instr->Bit(21) == 0) {
      if (instr->Bit(15) == 0) {
        if (instr->Bit(10) == 0) {
          if (instr->Bit(29) == 0) {
            if (instr->Bit(11) == 0) {
              VisitCrypto3RegSHA(instr);
            } else {
              VisitUnallocated(instr);
            }
          } else {
            VisitUnallocated(instr);
          }
        } else {
          if (instr->Bits(23, 22) == 0) {
            VisitNEONScalarCopy(instr);
          } else {
            VisitUnallocated(instr);
          }
        }
      } else {
        VisitUnallocated(instr);
      }
    } else {
      if (instr->Bit(10) == 0) {
        if (instr->Bit(11) == 0) {
          VisitNEONScalar3Diff(instr);
        } else {
          if (instr->Bits(18, 17) == 0) {
            if (instr->Bit(20) == 0) {
              if (instr->Bit(19) == 0) {
                VisitNEONScalar2RegMisc(instr);
              } else {
                if (instr->Bit(29) == 0) {
                  VisitCrypto2RegSHA(instr);
                } else {
                  VisitUnallocated(instr);
                }
              }
            } else {
              if (instr->Bit(19) == 0) {
                VisitNEONScalarPairwise(instr);
              } else {
                VisitUnallocated(instr);
              }
            }
          } else {
            VisitUnallocated(instr);
          }
        }
      } else {
        VisitNEONScalar3Same(instr);
      }
    }
  } else {
    if (instr->Bit(10) == 0) {
      VisitNEONScalarByIndexedElement(instr);
    } else {
      if (instr->Bit(23) == 0) {
        VisitNEONScalarShiftImmediate(instr);
      } else {
        VisitUnallocated(instr);
      }
    }
  }
}


#define DEFINE_VISITOR_CALLERS(A)                                              \
  void Decoder::Visit##A(const Instruction *instr) {                           \
    VIXL_ASSERT(instr->Mask(A##FMask) == A##Fixed);                            \
    for (auto visitor : visitors_) {                                           \
      visitor->Visit##A(instr);                                                \
    }                                                                          \
  }
VISITOR_LIST(DEFINE_VISITOR_CALLERS)
#undef DEFINE_VISITOR_CALLERS
}  // namespace vixl
