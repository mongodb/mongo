/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Florian Bernd

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * Defines the basic `ZydisDecodedInstruction` and `ZydisDecodedOperand` structs.
 */

#ifndef ZYDIS_INSTRUCTIONINFO_H
#define ZYDIS_INSTRUCTIONINFO_H

#include "zydis/Zycore/Types.h"
#include "zydis/Zydis/MetaInfo.h"
#include "zydis/Zydis/Mnemonic.h"
#include "zydis/Zydis/Register.h"
#include "zydis/Zydis/SharedTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Decoded operand                                                                                */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Operand attributes                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisOperandAttributes` data-type.
 */
typedef ZyanU8 ZydisOperandAttributes;

/**
 * The operand is a `MULTISOURCE4` register operand.
 *
 * This is a special register operand-type used by `4FMAPS` instructions where the given register
 * points to the first register of a register range (4 registers in total).
 *
 * Example: ZMM3 -> [ZMM3..ZMM6]
 */
#define ZYDIS_OATTRIB_IS_MULTISOURCE4   0x01 // (1 <<  0)

/* ---------------------------------------------------------------------------------------------- */
/* Memory type                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisMemoryOperandType` enum.
 */
typedef enum ZydisMemoryOperandType_
{
    ZYDIS_MEMOP_TYPE_INVALID,
    /**
     * Normal memory operand.
     */
    ZYDIS_MEMOP_TYPE_MEM,
    /**
     * The memory operand is only used for address-generation. No real memory-access is
     * caused.
     */
    ZYDIS_MEMOP_TYPE_AGEN,
    /**
     * A memory operand using `SIB` addressing form, where the index register is not used
     * in address calculation and scale is ignored. No real memory-access is caused.
     */
    ZYDIS_MEMOP_TYPE_MIB,
    /**
     * A vector `SIB` memory addressing operand (`VSIB`).
     */
    ZYDIS_MEMOP_TYPE_VSIB,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_MEMOP_TYPE_MAX_VALUE = ZYDIS_MEMOP_TYPE_VSIB,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MEMOP_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_MEMOP_TYPE_MAX_VALUE)
} ZydisMemoryOperandType;

/* ---------------------------------------------------------------------------------------------- */
/* Decoded operand                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Extended info for register-operands.
 */
typedef struct ZydisDecodedOperandReg_
{
    /**
     * The register value.
     */
    ZydisRegister value;
} ZydisDecodedOperandReg;

/**
 * Extended info for memory-operands.
 */
typedef struct ZydisDecodedOperandMem_
{
    /**
     * The type of the memory operand.
     */
    ZydisMemoryOperandType type;
    /**
     * The segment register.
     */
    ZydisRegister segment;
    /**
     * The base register.
     */
    ZydisRegister base;
    /**
     * The index register.
     */
    ZydisRegister index;
    /**
     * The scale factor.
     */
    ZyanU8 scale;
    /**
     * Extended info for memory-operands with displacement.
     */
    struct ZydisDecodedOperandMemDisp_
    {
        /**
         * Signals, if the displacement value is used.
         */
        ZyanBool has_displacement;
        /**
         * The displacement value
         */
        ZyanI64 value;
    } disp;
} ZydisDecodedOperandMem;

/**
 * Extended info for pointer-operands.
 */
typedef struct ZydisDecodedOperandPtr_
{
    ZyanU16 segment;
    ZyanU32 offset;
} ZydisDecodedOperandPtr;

/**
 * Extended info for immediate-operands.
 */
typedef struct ZydisDecodedOperandImm_
{
    /**
     * Signals, if the immediate value is signed.
     */
    ZyanBool is_signed;
    /**
     * Signals, if the immediate value contains a relative offset. You can use
     * `ZydisCalcAbsoluteAddress` to determine the absolute address value.
     */
    ZyanBool is_relative;
    /**
     * The immediate value.
     */
    union ZydisDecodedOperandImmValue_
    {
        ZyanU64 u;
        ZyanI64 s;
    } value;
} ZydisDecodedOperandImm;

/**
 * Defines the `ZydisDecodedOperand` struct.
 */
typedef struct ZydisDecodedOperand_
{
    /**
     * The operand-id.
     */
    ZyanU8 id;
    /**
     * The visibility of the operand.
     */
    ZydisOperandVisibility visibility;
    /**
     * The operand-actions.
     */
    ZydisOperandActions actions;
    /**
     * The operand-encoding.
     */
    ZydisOperandEncoding encoding;
    /**
     * The logical size of the operand (in bits).
     */
    ZyanU16 size;
    /**
     * The element-type.
     */
    ZydisElementType element_type;
    /**
     * The size of a single element.
     */
    ZydisElementSize element_size;
    /**
     * The number of elements.
     */
    ZyanU16 element_count;
    /*
     * Additional operand attributes.
     */
    ZydisOperandAttributes attributes;
    /**
     * The type of the operand.
     */
    ZydisOperandType type;
    /*
     * Operand type specific information.
     *
     * The enabled union variant is determined by the `type` field.
     */
    union
    {
        ZydisDecodedOperandReg reg;
        ZydisDecodedOperandMem mem;
        ZydisDecodedOperandPtr ptr;
        ZydisDecodedOperandImm imm;
    };
} ZydisDecodedOperand;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Decoded instruction                                                                            */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* CPU/FPU flags                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisAccessedFlagsMask` data-type.
 */
typedef ZyanU32 ZydisAccessedFlagsMask;

/**
 * @defgroup decoder_cpu_flags CPU flags
 * @ingroup decoder
 *
 * Constants used for testing CPU flags accessed by an instruction.
 *
 * @{
 */

/**
 * Carry flag.
 */
#define ZYDIS_CPUFLAG_CF    (1ul <<  0)
/**
 * Parity flag.
 */
#define ZYDIS_CPUFLAG_PF    (1ul <<  2)
/**
 * Adjust flag.
 */
#define ZYDIS_CPUFLAG_AF    (1ul <<  4)
/**
 * Zero flag.
 */
#define ZYDIS_CPUFLAG_ZF    (1ul <<  6)
/**
 * Sign flag.
 */
#define ZYDIS_CPUFLAG_SF    (1ul <<  7)
/**
 * Trap flag.
 */
#define ZYDIS_CPUFLAG_TF    (1ul <<  8)
/**
 * Interrupt enable flag.
 */
#define ZYDIS_CPUFLAG_IF    (1ul <<  9)
/**
 * Direction flag.
 */
#define ZYDIS_CPUFLAG_DF    (1ul << 10)
/**
 * Overflow flag.
 */
#define ZYDIS_CPUFLAG_OF    (1ul << 11)
/**
 * I/O privilege level flag.
 */
#define ZYDIS_CPUFLAG_IOPL  (1ul << 12)
/**
 * Nested task flag.
 */
#define ZYDIS_CPUFLAG_NT    (1ul << 14)
/**
 * Resume flag.
 */
#define ZYDIS_CPUFLAG_RF    (1ul << 16)
/**
 * Virtual 8086 mode flag.
 */
#define ZYDIS_CPUFLAG_VM    (1ul << 17)
/**
 * Alignment check.
 */
#define ZYDIS_CPUFLAG_AC    (1ul << 18)
/**
 * Virtual interrupt flag.
 */
#define ZYDIS_CPUFLAG_VIF   (1ul << 19)
/**
 * Virtual interrupt pending.
 */
#define ZYDIS_CPUFLAG_VIP   (1ul << 20)
/**
 * Able to use CPUID instruction.
 */
#define ZYDIS_CPUFLAG_ID    (1ul << 21)

/**
 * @}
 */

/**
 * @defgroup decoder_fpu_flags FPU flags
 * @ingroup decoder
 *
 * Constants used for testing FPU flags accessed by an instruction.
 *
 * @{
 */

/**
 * FPU condition-code flag 0.
 */
#define ZYDIS_FPUFLAG_C0    (1ul <<  0)
/**
 * FPU condition-code flag 1.
 */
#define ZYDIS_FPUFLAG_C1    (1ul <<  1)
 /**
  * FPU condition-code flag 2.
  */
#define ZYDIS_FPUFLAG_C2    (1ul <<  2)
/**
 * FPU condition-code flag 3.
 */
#define ZYDIS_FPUFLAG_C3    (1ul <<  3)

/**
 * @}
 */

/*
 * Information about CPU/FPU flags accessed by the instruction.
 */
typedef struct ZydisAccessedFlags_
{
    /*
     * As mask containing the flags `TESTED` by the instruction.
     */
    ZydisAccessedFlagsMask tested;
    /*
     * As mask containing the flags `MODIFIED` by the instruction.
     */
    ZydisAccessedFlagsMask modified;
    /*
     * As mask containing the flags `SET_0` by the instruction.
     */
    ZydisAccessedFlagsMask set_0;
    /*
     * As mask containing the flags `SET_1` by the instruction.
     */
    ZydisAccessedFlagsMask set_1;
    /*
     * As mask containing the flags `UNDEFINED` by the instruction.
     */
    ZydisAccessedFlagsMask undefined;
} ZydisAccessedFlags;

/* ---------------------------------------------------------------------------------------------- */
/* Branch types                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisBranchType` enum.
 */
typedef enum ZydisBranchType_
{
    /**
     * The instruction is not a branch instruction.
     */
    ZYDIS_BRANCH_TYPE_NONE,
    /**
     * The instruction is a short (8-bit) branch instruction.
     */
    ZYDIS_BRANCH_TYPE_SHORT,
    /**
     * The instruction is a near (16-bit or 32-bit) branch instruction.
     */
    ZYDIS_BRANCH_TYPE_NEAR,
    /**
     * The instruction is a far (inter-segment) branch instruction.
     */
    ZYDIS_BRANCH_TYPE_FAR,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_BRANCH_TYPE_MAX_VALUE = ZYDIS_BRANCH_TYPE_FAR,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_BRANCH_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_BRANCH_TYPE_MAX_VALUE)
} ZydisBranchType;

/* ---------------------------------------------------------------------------------------------- */
/* SSE/AVX exception-class                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisExceptionClass` enum.
 */
typedef enum ZydisExceptionClass_
{
    ZYDIS_EXCEPTION_CLASS_NONE,
    // TODO: FP Exceptions
    ZYDIS_EXCEPTION_CLASS_SSE1,
    ZYDIS_EXCEPTION_CLASS_SSE2,
    ZYDIS_EXCEPTION_CLASS_SSE3,
    ZYDIS_EXCEPTION_CLASS_SSE4,
    ZYDIS_EXCEPTION_CLASS_SSE5,
    ZYDIS_EXCEPTION_CLASS_SSE7,
    ZYDIS_EXCEPTION_CLASS_AVX1,
    ZYDIS_EXCEPTION_CLASS_AVX2,
    ZYDIS_EXCEPTION_CLASS_AVX3,
    ZYDIS_EXCEPTION_CLASS_AVX4,
    ZYDIS_EXCEPTION_CLASS_AVX5,
    ZYDIS_EXCEPTION_CLASS_AVX6,
    ZYDIS_EXCEPTION_CLASS_AVX7,
    ZYDIS_EXCEPTION_CLASS_AVX8,
    ZYDIS_EXCEPTION_CLASS_AVX11,
    ZYDIS_EXCEPTION_CLASS_AVX12,
    ZYDIS_EXCEPTION_CLASS_E1,
    ZYDIS_EXCEPTION_CLASS_E1NF,
    ZYDIS_EXCEPTION_CLASS_E2,
    ZYDIS_EXCEPTION_CLASS_E2NF,
    ZYDIS_EXCEPTION_CLASS_E3,
    ZYDIS_EXCEPTION_CLASS_E3NF,
    ZYDIS_EXCEPTION_CLASS_E4,
    ZYDIS_EXCEPTION_CLASS_E4NF,
    ZYDIS_EXCEPTION_CLASS_E5,
    ZYDIS_EXCEPTION_CLASS_E5NF,
    ZYDIS_EXCEPTION_CLASS_E6,
    ZYDIS_EXCEPTION_CLASS_E6NF,
    ZYDIS_EXCEPTION_CLASS_E7NM,
    ZYDIS_EXCEPTION_CLASS_E7NM128,
    ZYDIS_EXCEPTION_CLASS_E9NF,
    ZYDIS_EXCEPTION_CLASS_E10,
    ZYDIS_EXCEPTION_CLASS_E10NF,
    ZYDIS_EXCEPTION_CLASS_E11,
    ZYDIS_EXCEPTION_CLASS_E11NF,
    ZYDIS_EXCEPTION_CLASS_E12,
    ZYDIS_EXCEPTION_CLASS_E12NP,
    ZYDIS_EXCEPTION_CLASS_K20,
    ZYDIS_EXCEPTION_CLASS_K21,
    ZYDIS_EXCEPTION_CLASS_AMXE1,
    ZYDIS_EXCEPTION_CLASS_AMXE2,
    ZYDIS_EXCEPTION_CLASS_AMXE3,
    ZYDIS_EXCEPTION_CLASS_AMXE4,
    ZYDIS_EXCEPTION_CLASS_AMXE5,
    ZYDIS_EXCEPTION_CLASS_AMXE6,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_EXCEPTION_CLASS_MAX_VALUE = ZYDIS_EXCEPTION_CLASS_AMXE6,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_EXCEPTION_CLASS_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_EXCEPTION_CLASS_MAX_VALUE)
} ZydisExceptionClass;

/* ---------------------------------------------------------------------------------------------- */
/* AVX mask mode                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisMaskMode` enum.
 */
typedef enum ZydisMaskMode_
{
    ZYDIS_MASK_MODE_INVALID,
    /**
     * Masking is disabled for the current instruction (`K0` register is used).
     */
    ZYDIS_MASK_MODE_DISABLED,
    /**
     * The embedded mask register is used as a merge-mask.
     */
    ZYDIS_MASK_MODE_MERGING,
    /**
     * The embedded mask register is used as a zero-mask.
     */
    ZYDIS_MASK_MODE_ZEROING,
    /**
     * The embedded mask register is used as a control-mask (element selector).
     */
    ZYDIS_MASK_MODE_CONTROL,
    /**
     * The embedded mask register is used as a zeroing control-mask (element selector).
     */
    ZYDIS_MASK_MODE_CONTROL_ZEROING,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_MASK_MODE_MAX_VALUE = ZYDIS_MASK_MODE_CONTROL_ZEROING,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MASK_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_MASK_MODE_MAX_VALUE)
} ZydisMaskMode;

/* ---------------------------------------------------------------------------------------------- */
/* AVX broadcast-mode                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisBroadcastMode` enum.
 */
typedef enum ZydisBroadcastMode_
{
    ZYDIS_BROADCAST_MODE_INVALID,
    ZYDIS_BROADCAST_MODE_1_TO_2,
    ZYDIS_BROADCAST_MODE_1_TO_4,
    ZYDIS_BROADCAST_MODE_1_TO_8,
    ZYDIS_BROADCAST_MODE_1_TO_16,
    ZYDIS_BROADCAST_MODE_1_TO_32,
    ZYDIS_BROADCAST_MODE_1_TO_64,
    ZYDIS_BROADCAST_MODE_2_TO_4,
    ZYDIS_BROADCAST_MODE_2_TO_8,
    ZYDIS_BROADCAST_MODE_2_TO_16,
    ZYDIS_BROADCAST_MODE_4_TO_8,
    ZYDIS_BROADCAST_MODE_4_TO_16,
    ZYDIS_BROADCAST_MODE_8_TO_16,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_BROADCAST_MODE_MAX_VALUE = ZYDIS_BROADCAST_MODE_8_TO_16,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_BROADCAST_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_BROADCAST_MODE_MAX_VALUE)
} ZydisBroadcastMode;

/* ---------------------------------------------------------------------------------------------- */
/* AVX rounding-mode                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisRoundingMode` enum.
 */
typedef enum ZydisRoundingMode_
{
    ZYDIS_ROUNDING_MODE_INVALID,
    /**
     * Round to nearest.
     */
    ZYDIS_ROUNDING_MODE_RN,
    /**
     * Round down.
     */
    ZYDIS_ROUNDING_MODE_RD,
    /**
     * Round up.
     */
    ZYDIS_ROUNDING_MODE_RU,
    /**
     * Round towards zero.
     */
    ZYDIS_ROUNDING_MODE_RZ,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_ROUNDING_MODE_MAX_VALUE = ZYDIS_ROUNDING_MODE_RZ,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_ROUNDING_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_ROUNDING_MODE_MAX_VALUE)
} ZydisRoundingMode;

/* ---------------------------------------------------------------------------------------------- */
/* KNC swizzle-mode                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisSwizzleMode` enum.
 */
typedef enum ZydisSwizzleMode_
{
    ZYDIS_SWIZZLE_MODE_INVALID,
    ZYDIS_SWIZZLE_MODE_DCBA,
    ZYDIS_SWIZZLE_MODE_CDAB,
    ZYDIS_SWIZZLE_MODE_BADC,
    ZYDIS_SWIZZLE_MODE_DACB,
    ZYDIS_SWIZZLE_MODE_AAAA,
    ZYDIS_SWIZZLE_MODE_BBBB,
    ZYDIS_SWIZZLE_MODE_CCCC,
    ZYDIS_SWIZZLE_MODE_DDDD,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_SWIZZLE_MODE_MAX_VALUE = ZYDIS_SWIZZLE_MODE_DDDD,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_SWIZZLE_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_SWIZZLE_MODE_MAX_VALUE)
} ZydisSwizzleMode;

/* ---------------------------------------------------------------------------------------------- */
/* KNC conversion-mode                                                                            */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisConversionMode` enum.
 */
typedef enum ZydisConversionMode_
{
    ZYDIS_CONVERSION_MODE_INVALID,
    ZYDIS_CONVERSION_MODE_FLOAT16,
    ZYDIS_CONVERSION_MODE_SINT8,
    ZYDIS_CONVERSION_MODE_UINT8,
    ZYDIS_CONVERSION_MODE_SINT16,
    ZYDIS_CONVERSION_MODE_UINT16,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_CONVERSION_MODE_MAX_VALUE = ZYDIS_CONVERSION_MODE_UINT16,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_CONVERSION_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_CONVERSION_MODE_MAX_VALUE)
} ZydisConversionMode;

/* ---------------------------------------------------------------------------------------------- */
/* Legacy prefix type                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisPrefixType` enum.
 */
typedef enum ZydisPrefixType_
{
    /**
     * The prefix is ignored by the instruction.
     *
     * This applies to all prefixes that are not accepted by the instruction in general or the
     * ones that are overwritten by a prefix of the same group closer to the instruction opcode.
     */
    ZYDIS_PREFIX_TYPE_IGNORED,
    /**
     * The prefix is effectively used by the instruction.
     */
    ZYDIS_PREFIX_TYPE_EFFECTIVE,
    /**
     * The prefix is used as a mandatory prefix.
     *
     * A mandatory prefix is interpreted as an opcode extension and has no further effect on the
     * instruction.
     */
    ZYDIS_PREFIX_TYPE_MANDATORY,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_PREFIX_TYPE_MAX_VALUE = ZYDIS_PREFIX_TYPE_MANDATORY,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_PREFIX_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_PREFIX_TYPE_MAX_VALUE)
} ZydisPrefixType;

// TODO: Check effective for 66/67 prefixes (currently defaults to EFFECTIVE)

/* ---------------------------------------------------------------------------------------------- */
/* Decoded instruction                                                                            */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Detailed info about the `REX` prefix.
 */
typedef struct ZydisDecodedInstructionRawRex_
{
    /**
     * 64-bit operand-size promotion.
     */
    ZyanU8 W;
    /**
     * Extension of the `ModRM.reg` field.
     */
    ZyanU8 R;
    /**
     * Extension of the `SIB.index` field.
     */
    ZyanU8 X;
    /**
     * Extension of the `ModRM.rm`, `SIB.base`, or `opcode.reg` field.
     */
    ZyanU8 B;
    /**
     * The offset of the effective `REX` byte, relative to the beginning of the
     * instruction, in bytes.
     *
     * This offset always points to the "effective" `REX` prefix (the one closest to the
     * instruction opcode), if multiple `REX` prefixes are present.
     *
     * Note that the `REX` byte can be the first byte of the instruction, which would lead
     * to an offset of `0`. Please refer to the instruction attributes to check for the
     * presence of the `REX` prefix.
     */
    ZyanU8 offset;
} ZydisDecodedInstructionRawRex;

/**
 * Detailed info about the `XOP` prefix.
 */
typedef struct ZydisDecodedInstructionRawXop_
{
    /**
     * Extension of the `ModRM.reg` field (inverted).
     */
    ZyanU8 R;
    /**
     * Extension of the `SIB.index` field (inverted).
     */
    ZyanU8 X;
    /**
     * Extension of the `ModRM.rm`, `SIB.base`, or `opcode.reg` field (inverted).
     */
    ZyanU8 B;
    /**
     * Opcode-map specifier.
     */
    ZyanU8 m_mmmm;
    /**
     * 64-bit operand-size promotion or opcode-extension.
     */
    ZyanU8 W;
    /**
     * `NDS`/`NDD` (non-destructive-source/destination) register
     * specifier (inverted).
     */
    ZyanU8 vvvv;
    /**
     * Vector-length specifier.
     */
    ZyanU8 L;
    /**
     * Compressed legacy prefix.
     */
    ZyanU8 pp;
    /**
     * The offset of the first xop byte, relative to the beginning of
     * the instruction, in bytes.
     */
    ZyanU8 offset;
} ZydisDecodedInstructionRawXop;

/**
 * Detailed info about the `VEX` prefix.
 */
typedef struct ZydisDecodedInstructionRawVex_
{
    /**
     * Extension of the `ModRM.reg` field (inverted).
     */
    ZyanU8 R;
    /**
     * Extension of the `SIB.index` field (inverted).
     */
    ZyanU8 X;
    /**
     * Extension of the `ModRM.rm`, `SIB.base`, or `opcode.reg` field (inverted).
     */
    ZyanU8 B;
    /**
     * Opcode-map specifier.
     */
    ZyanU8 m_mmmm;
    /**
     * 64-bit operand-size promotion or opcode-extension.
     */
    ZyanU8 W;
    /**
     * `NDS`/`NDD` (non-destructive-source/destination) register specifier
     *  (inverted).
     */
    ZyanU8 vvvv;
    /**
     * Vector-length specifier.
     */
    ZyanU8 L;
    /**
     * Compressed legacy prefix.
     */
    ZyanU8 pp;
    /**
     * The offset of the first `VEX` byte, relative to the beginning of the instruction, in
     * bytes.
     */
    ZyanU8 offset;
    /**
     * The size of the `VEX` prefix, in bytes.
     */
    ZyanU8 size;
} ZydisDecodedInstructionRawVex;

/**
 * Detailed info about the `EVEX` prefix.
 */
typedef struct ZydisDecodedInstructionRawEvex
{
    /**
     * Extension of the `ModRM.reg` field (inverted).
     */
    ZyanU8 R;
    /**
     * Extension of the `SIB.index/vidx` field (inverted).
     */
    ZyanU8 X;
    /**
     * Extension of the `ModRM.rm` or `SIB.base` field (inverted).
     */
    ZyanU8 B;
    /**
     * High-16 register specifier modifier (inverted).
     */
    ZyanU8 R2;
    /**
     * Opcode-map specifier.
     */
    ZyanU8 mmm;
    /**
     * 64-bit operand-size promotion or opcode-extension.
     */
    ZyanU8 W;
    /**
     * `NDS`/`NDD` (non-destructive-source/destination) register specifier
     * (inverted).
     */
    ZyanU8 vvvv;
    /**
     * Compressed legacy prefix.
     */
    ZyanU8 pp;
    /**
     * Zeroing/Merging.
     */
    ZyanU8 z;
    /**
     * Vector-length specifier or rounding-control (most significant bit).
     */
    ZyanU8 L2;
    /**
     * Vector-length specifier or rounding-control (least significant bit).
     */
    ZyanU8 L;
    /**
     * Broadcast/RC/SAE context.
     */
    ZyanU8 b;
    /**
     * High-16 `NDS`/`VIDX` register specifier.
     */
    ZyanU8 V2;
    /**
     * Embedded opmask register specifier.
     */
    ZyanU8 aaa;
    /**
     * The offset of the first evex byte, relative to the beginning of the
     * instruction, in bytes.
     */
    ZyanU8 offset;
} ZydisDecodedInstructionRawEvex;

/**
 * Detailed info about the `MVEX` prefix.
 */
typedef struct ZydisDecodedInstructionRawMvex_
{
    /**
     * Extension of the `ModRM.reg` field (inverted).
     */
    ZyanU8 R;
    /**
     * Extension of the `SIB.index/vidx` field (inverted).
     */
    ZyanU8 X;
    /**
     * Extension of the `ModRM.rm` or `SIB.base` field (inverted).
     */
    ZyanU8 B;
    /**
     * High-16 register specifier modifier (inverted).
     */
    ZyanU8 R2;
    /**
     * Opcode-map specifier.
     */
    ZyanU8 mmmm;
    /**
     * 64-bit operand-size promotion or opcode-extension.
     */
    ZyanU8 W;
    /**
     * `NDS`/`NDD` (non-destructive-source/destination) register specifier
     *  (inverted).
     */
    ZyanU8 vvvv;
    /**
     * Compressed legacy prefix.
     */
    ZyanU8 pp;
    /**
     * Non-temporal/eviction hint.
     */
    ZyanU8 E;
    /**
     * Swizzle/broadcast/up-convert/down-convert/static-rounding controls.
     */
    ZyanU8 SSS;
    /**
     * High-16 `NDS`/`VIDX` register specifier.
     */
    ZyanU8 V2;
    /**
     * Embedded opmask register specifier.
     */
    ZyanU8 kkk;
    /**
     * The offset of the first mvex byte, relative to the beginning of the
     * instruction, in bytes.
     */
    ZyanU8 offset;
} ZydisDecodedInstructionRawMvex;

/**
 * Extended info for `AVX` instructions.
 */
typedef struct ZydisDecodedInstructionAvx_
{
    /**
     * The `AVX` vector-length.
     */
    ZyanU16 vector_length;
    /**
     * Info about the embedded writemask-register (`AVX-512` and `KNC` only).
     */
    struct ZydisDecodedInstructionAvxMask_
    {
        /**
         * The masking mode.
         */
        ZydisMaskMode mode;
        /**
         * The mask register.
         */
        ZydisRegister reg;
    } mask;
    /**
     * Contains info about the `AVX` broadcast.
     */
    struct ZydisDecodedInstructionAvxBroadcast_
    {
        /**
         * Signals, if the broadcast is a static broadcast.
         *
         * This is the case for instructions with inbuilt broadcast functionality, which is
         * always active and not controlled by the `EVEX/MVEX.RC` bits.
         */
        ZyanBool is_static;
        /**
         * The `AVX` broadcast-mode.
         */
        ZydisBroadcastMode mode;
    } broadcast;
    /**
     * Contains info about the `AVX` rounding.
     */
    struct ZydisDecodedInstructionAvxRounding_
    {
        /**
         * The `AVX` rounding-mode.
         */
        ZydisRoundingMode mode;
    } rounding;
    /**
     * Contains info about the `AVX` register-swizzle (`KNC` only).
     */
    struct ZydisDecodedInstructionAvxSwizzle_
    {
        /**
         * The `AVX` register-swizzle mode.
         */
        ZydisSwizzleMode mode;
    } swizzle;
    /**
     * Contains info about the `AVX` data-conversion (`KNC` only).
     */
    struct ZydisDecodedInstructionAvxConversion_
    {
        /**
         * The `AVX` data-conversion mode.
         */
        ZydisConversionMode mode;
    } conversion;
    /**
     * Signals, if the `SAE` (suppress-all-exceptions) functionality is
     * enabled for the instruction.
     */
    ZyanBool has_sae;
    /**
     * Signals, if the instruction has a memory-eviction-hint (`KNC` only).
     */
    ZyanBool has_eviction_hint;
    // TODO: publish EVEX tuple-type and MVEX functionality
} ZydisDecodedInstructionAvx;

/**
 * Instruction meta info.
 */
typedef struct ZydisDecodedInstructionMeta_
{
    /**
     * The instruction category.
     */
    ZydisInstructionCategory category;
    /**
     * The ISA-set.
     */
    ZydisISASet isa_set;
    /**
     * The ISA-set extension.
     */
    ZydisISAExt isa_ext;
    /**
     * The branch type.
     */
    ZydisBranchType branch_type;
    /**
     * The exception class.
     */
    ZydisExceptionClass exception_class;
} ZydisDecodedInstructionMeta;

/**
 * Detailed info about different instruction-parts like `ModRM`, `SIB` or
 * encoding-prefixes.
 */
typedef struct ZydisDecodedInstructionRaw_
{
    /**
     * The number of legacy prefixes.
     */
    ZyanU8 prefix_count;
    /**
     * Detailed info about the legacy prefixes (including `REX`).
     */
    struct ZydisDecodedInstructionRawPrefixes_
    {
        /**
         * The prefix type.
         */
        ZydisPrefixType type;
        /**
         * The prefix byte.
         */
        ZyanU8 value;
    } prefixes[ZYDIS_MAX_INSTRUCTION_LENGTH];

    /*
     * Copy of the `encoding` field.
     *
     * This is here to allow the Rust bindings to treat the following union as an `enum`,
     * sparing us a lot of unsafe code. Prefer using the regular `encoding` field in C/C++ code.
     */
    ZydisInstructionEncoding encoding2;
    /*
     * Union for things from various mutually exclusive encodings.
     */
    union
    {
        ZydisDecodedInstructionRawRex rex;
        ZydisDecodedInstructionRawXop xop;
        ZydisDecodedInstructionRawVex vex;
        ZydisDecodedInstructionRawEvex evex;
        ZydisDecodedInstructionRawMvex mvex;
    };

    /**
     * Detailed info about the `ModRM` byte.
     */
    struct ZydisDecodedInstructionModRm_
    {
        /**
         * The addressing mode.
         */
        ZyanU8 mod;
        /**
         * Register specifier or opcode-extension.
         */
        ZyanU8 reg;
        /**
         * Register specifier or opcode-extension.
         */
        ZyanU8 rm;
        /**
         * The offset of the `ModRM` byte, relative to the beginning of the
         * instruction, in bytes.
         */
        ZyanU8 offset;
    } modrm;
    /**
     * Detailed info about the `SIB` byte.
     */
    struct ZydisDecodedInstructionRawSib_
    {
        /**
         * The scale factor.
         */
        ZyanU8 scale;
        /**
         * The index-register specifier.
         */
        ZyanU8 index;
        /**
         * The base-register specifier.
         */
        ZyanU8 base;
        /**
         * The offset of the `SIB` byte, relative to the beginning of the
         * instruction, in bytes.
         */
        ZyanU8 offset;
    } sib;
    /**
     * Detailed info about displacement-bytes.
     */
    struct ZydisDecodedInstructionRawDisp_
    {
        /**
         * The displacement value
         */
        ZyanI64 value;
        /**
         * The physical displacement size, in bits.
         */
        ZyanU8 size;
        // TODO: publish cd8 scale
        /**
         * The offset of the displacement data, relative to the beginning of the
         * instruction, in bytes.
         */
        ZyanU8 offset;
    } disp;
    /**
     * Detailed info about immediate-bytes.
     */
    struct ZydisDecodedInstructionRawImm_
    {
        /**
         * Signals, if the immediate value is signed.
         */
        ZyanBool is_signed;
        /**
         * Signals, if the immediate value contains a relative offset. You can use
         * `ZydisCalcAbsoluteAddress` to determine the absolute address value.
         */
        ZyanBool is_relative;
        /**
         * The immediate value.
         */
        union ZydisDecodedInstructionRawImmValue_
        {
            ZyanU64 u;
            ZyanI64 s;
        } value;
        /**
         * The physical immediate size, in bits.
         */
        ZyanU8 size;
        /**
         * The offset of the immediate data, relative to the beginning of the
         * instruction, in bytes.
         */
        ZyanU8 offset;
    } imm[2];
} ZydisDecodedInstructionRaw;

/**
 * Information about a decoded instruction.
 */
typedef struct ZydisDecodedInstruction_
{
    /**
     * The machine mode used to decode this instruction.
     */
    ZydisMachineMode machine_mode;
    /**
     * The instruction-mnemonic.
     */
    ZydisMnemonic mnemonic;
    /**
     * The length of the decoded instruction.
     */
    ZyanU8 length;
    /**
     * The instruction-encoding (`LEGACY`, `3DNOW`, `VEX`, `EVEX`, `XOP`).
     */
    ZydisInstructionEncoding encoding;
    /**
     * The opcode-map.
     */
    ZydisOpcodeMap opcode_map;
    /**
     * The instruction-opcode.
     */
    ZyanU8 opcode;
    /**
     * The stack width.
     */
    ZyanU8 stack_width;
    /**
     * The effective operand width.
     */
    ZyanU8 operand_width;
    /**
     * The effective address width.
     */
    ZyanU8 address_width;
    /**
     * The number of instruction-operands.
     *
     * Explicit and implicit operands are guaranteed to be in the front and ordered as they are
     * printed by the formatter in `Intel` mode. No assumptions can be made about the order of
     * hidden operands, except that they always located behind the explicit and implicit operands.
     */
    ZyanU8 operand_count;
    /**
     * The number of explicit (visible) instruction-operands.
     *
     * Explicit and implicit operands are guaranteed to be in the front and ordered as they are
     * printed by the formatter in `Intel` mode.
     */
    ZyanU8 operand_count_visible;
    /**
     * See @ref instruction_attributes.
     */
    ZydisInstructionAttributes attributes;
    /**
     * Information about CPU flags accessed by the instruction.
     *
     * The bits in the masks correspond to the actual bits in the `FLAGS/EFLAGS/RFLAGS`
     * register. See @ref decoder_cpu_flags.
     */
    const ZydisAccessedFlags* cpu_flags;
    /**
     * Information about FPU flags accessed by the instruction.
     * 
     * See @ref decoder_fpu_flags.
     */
    const ZydisAccessedFlags* fpu_flags;
    /**
     * Extended info for `AVX` instructions.
     */
    ZydisDecodedInstructionAvx avx;
    /**
     * Meta info.
     */
    ZydisDecodedInstructionMeta meta;
    /**
     * Detailed info about different instruction-parts like `ModRM`, `SIB` or
     * encoding-prefixes.
     */
    ZydisDecodedInstructionRaw raw;
} ZydisDecodedInstruction;

/* ---------------------------------------------------------------------------------------------- */
/* Decoder context                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * The decoder context is used to preserve some internal state between subsequent decode
 * operations for THE SAME instruction.
 *
 * The context is initialized by @c ZydisDecoderDecodeInstruction and required by e.g.
 * @c ZydisDecoderDecodeOperands.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 *
 * This struct is neither ABI nor API stable!
 */
typedef struct ZydisDecoderContext_
{
    /**
     * A pointer to the internal instruction definition.
     */
    const void* definition;
    /**
     * Contains the effective operand-size index.
     *
     * 0 = 16 bit, 1 = 32 bit, 2 = 64 bit
     */
    ZyanU8 eosz_index;
    /**
     * Contains the effective address-size index.
     *
     * 0 = 16 bit, 1 = 32 bit, 2 = 64 bit
     */
    ZyanU8 easz_index;
    /**
     * Contains some cached REX/XOP/VEX/EVEX/MVEX values to provide uniform access.
     */
    struct
    {
        ZyanU8 W;
        ZyanU8 R;
        ZyanU8 X;
        ZyanU8 B;
        ZyanU8 L;
        ZyanU8 LL;
        ZyanU8 R2;
        ZyanU8 V2;
        ZyanU8 vvvv;
        ZyanU8 mask;
    } vector_unified;
    /**
     * Information about encoded operand registers.
     */
    struct
    {
        /**
         * Signals if the `modrm.mod == 3` or `reg` form is forced for the instruction.
         */
        ZyanBool is_mod_reg;
        /**
         * The final register id for the `reg` encoded register.
         */
        ZyanU8 id_reg;
        /**
         * The final register id for the `rm` encoded register.
         *
         * This value is only set, if a register is encoded in `modrm.rm`.
         */
        ZyanU8 id_rm;
        /**
         * The final register id for the `ndsndd` (`.vvvv`) encoded register.
         */
        ZyanU8 id_ndsndd;
        /**
         * The final register id for the base register.
         *
         * This value is only set, if a memory operand is encoded in `modrm.rm`.
         */
        ZyanU8 id_base;
        /**
         * The final register id for the index register.
         *
         * This value is only set, if a memory operand is encoded in `modrm.rm` and the `SIB` byte
         * is present.
         */
        ZyanU8 id_index;
    } reg_info;
    /**
     * Internal EVEX-specific information.
     */
    struct
    {
        /**
         * The EVEX tuple-type.
         */
        ZyanU8 tuple_type;
        /**
         * The EVEX element-size.
         */
        ZyanU8 element_size;
    } evex;
    /**
     * Internal MVEX-specific information.
     */
    struct
    {
        /**
         * The MVEX functionality.
         */
        ZyanU8 functionality;
    } mvex;
    /**
     * The scale factor for EVEX/MVEX compressed 8-bit displacement values.
     */
    ZyanU8 cd8_scale; // TODO: Could make sense to expose this in the ZydisDecodedInstruction
} ZydisDecoderContext;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_INSTRUCTIONINFO_H */
