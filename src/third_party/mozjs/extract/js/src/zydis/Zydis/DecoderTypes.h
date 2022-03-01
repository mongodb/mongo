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
 * @brief   Defines the basic `ZydisDecodedInstruction` and `ZydisDecodedOperand` structs.
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
/* Memory type                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisMemoryOperandType` enum.
 */
typedef enum ZydisMemoryOperandType_
{
    ZYDIS_MEMOP_TYPE_INVALID,
    /**
     * @brief   Normal memory operand.
     */
    ZYDIS_MEMOP_TYPE_MEM,
    /**
     * @brief   The memory operand is only used for address-generation. No real memory-access is
     *          caused.
     */
    ZYDIS_MEMOP_TYPE_AGEN,
    /**
     * @brief   A memory operand using `SIB` addressing form, where the index register is not used
     *          in address calculation and scale is ignored. No real memory-access is caused.
     */
    ZYDIS_MEMOP_TYPE_MIB,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_MEMOP_TYPE_MAX_VALUE = ZYDIS_MEMOP_TYPE_MIB,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MEMOP_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_MEMOP_TYPE_MAX_VALUE)
} ZydisMemoryOperandType;

/* ---------------------------------------------------------------------------------------------- */
/* Decoded operand                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisDecodedOperand` struct.
 */
typedef struct ZydisDecodedOperand_
{
    /**
     * @brief   The operand-id.
     */
    ZyanU8 id;
    /**
     * @brief   The type of the operand.
     */
    ZydisOperandType type;
    /**
     * @brief   The visibility of the operand.
     */
    ZydisOperandVisibility visibility;
    /**
     * @brief   The operand-actions.
     */
    ZydisOperandActions actions;
    /**
     * @brief   The operand-encoding.
     */
    ZydisOperandEncoding encoding;
    /**
     * @brief   The logical size of the operand (in bits).
     */
    ZyanU16 size;
    /**
     * @brief   The element-type.
     */
    ZydisElementType element_type;
    /**
     * @brief   The size of a single element.
     */
    ZydisElementSize element_size;
    /**
     * @brief   The number of elements.
     */
    ZyanU16 element_count;
    /**
     * @brief   Extended info for register-operands.
     */
    struct ZydisDecodedOperandReg_
    {
        /**
         * @brief   The register value.
         */
        ZydisRegister value;
        // TODO: AVX512_4VNNIW MULTISOURCE registers
    } reg;
    /**
     * @brief   Extended info for memory-operands.
     */
    struct ZydisDecodedOperandMem_
    {
        /**
         * @brief   The type of the memory operand.
         */
        ZydisMemoryOperandType type;
        /**
         * @brief   The segment register.
         */
        ZydisRegister segment;
        /**
         * @brief   The base register.
         */
        ZydisRegister base;
        /**
         * @brief   The index register.
         */
        ZydisRegister index;
        /**
         * @brief   The scale factor.
         */
        ZyanU8 scale;
        /**
         * @brief   Extended info for memory-operands with displacement.
         */
        struct ZydisDecodedOperandMemDisp_
        {
            /**
             * @brief   Signals, if the displacement value is used.
             */
            ZyanBool has_displacement;
            /**
             * @brief   The displacement value
             */
            ZyanI64 value;
        } disp;
    } mem;
    /**
     * @brief   Extended info for pointer-operands.
     */
    struct ZydisDecodedOperandPtr_
    {
        ZyanU16 segment;
        ZyanU32 offset;
    } ptr;
    /**
     * @brief   Extended info for immediate-operands.
     */
    struct ZydisDecodedOperandImm_
    {
        /**
         * @brief   Signals, if the immediate value is signed.
         */
        ZyanBool is_signed;
        /**
         * @brief   Signals, if the immediate value contains a relative offset. You can use
         *          `ZydisCalcAbsoluteAddress` to determine the absolute address value.
         */
        ZyanBool is_relative;
        /**
         * @brief   The immediate value.
         */
        union ZydisDecodedOperandImmValue_
        {
            ZyanU64 u;
            ZyanI64 s;
        } value;
    } imm;
} ZydisDecodedOperand;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Decoded instruction                                                                            */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Instruction attributes                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisInstructionAttributes` data-type.
 */
typedef ZyanU64 ZydisInstructionAttributes;

/**
 * @brief   The instruction has the `ModRM` byte.
 */
#define ZYDIS_ATTRIB_HAS_MODRM                  0x0000000000000001 // (1 <<  0)
/**
 * @brief   The instruction has the `SIB` byte.
 */
#define ZYDIS_ATTRIB_HAS_SIB                    0x0000000000000002 // (1 <<  1)
/**
 * @brief   The instruction has the `REX` prefix.
 */
#define ZYDIS_ATTRIB_HAS_REX                    0x0000000000000004 // (1 <<  2)
/**
 * @brief   The instruction has the `XOP` prefix.
 */
#define ZYDIS_ATTRIB_HAS_XOP                    0x0000000000000008 // (1 <<  3)
/**
 * @brief   The instruction has the `VEX` prefix.
 */
#define ZYDIS_ATTRIB_HAS_VEX                    0x0000000000000010 // (1 <<  4)
/**
 * @brief   The instruction has the `EVEX` prefix.
 */
#define ZYDIS_ATTRIB_HAS_EVEX                   0x0000000000000020 // (1 <<  5)
/**
 * @brief   The instruction has the `MVEX` prefix.
 */
#define ZYDIS_ATTRIB_HAS_MVEX                   0x0000000000000040 // (1 <<  6)
/**
 * @brief   The instruction has one or more operands with position-relative offsets.
 */
#define ZYDIS_ATTRIB_IS_RELATIVE                0x0000000000000080 // (1 <<  7)
/**
 * @brief   The instruction is privileged.
 *
 * Privileged instructions are any instructions that require a current ring level below 3.
 */
#define ZYDIS_ATTRIB_IS_PRIVILEGED              0x0000000000000100 // (1 <<  8)

/**
 * @brief   The instruction accesses one or more CPU-flags.
 */
#define ZYDIS_ATTRIB_CPUFLAG_ACCESS             0x0000001000000000 // (1 << 36) // TODO: rebase

/**
 * @brief   The instruction may conditionally read the general CPU state.
 */
#define ZYDIS_ATTRIB_CPU_STATE_CR               0x0000002000000000 // (1 << 37) // TODO: rebase
/**
 * @brief   The instruction may conditionally write the general CPU state.
 */
#define ZYDIS_ATTRIB_CPU_STATE_CW               0x0000004000000000 // (1 << 38) // TODO: rebase
/**
 * @brief   The instruction may conditionally read the FPU state (X87, MMX).
 */
#define ZYDIS_ATTRIB_FPU_STATE_CR               0x0000008000000000 // (1 << 39) // TODO: rebase
/**
 * @brief   The instruction may conditionally write the FPU state (X87, MMX).
 */
#define ZYDIS_ATTRIB_FPU_STATE_CW               0x0000010000000000 // (1 << 40) // TODO: rebase
/**
 * @brief   The instruction may conditionally read the XMM state (AVX, AVX2, AVX-512).
 */
#define ZYDIS_ATTRIB_XMM_STATE_CR               0x0000020000000000 // (1 << 41) // TODO: rebase
/**
 * @brief   The instruction may conditionally write the XMM state (AVX, AVX2, AVX-512).
 */
#define ZYDIS_ATTRIB_XMM_STATE_CW               0x0000040000000000 // (1 << 42) // TODO: rebase

/**
 * @brief   The instruction accepts the `LOCK` prefix (`0xF0`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_LOCK               0x0000000000000200 // (1 <<  9)
/**
 * @brief   The instruction accepts the `REP` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REP                0x0000000000000400 // (1 << 10)
/**
 * @brief   The instruction accepts the `REPE`/`REPZ` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REPE               0x0000000000000800 // (1 << 11)
/**
 * @brief   The instruction accepts the `REPE`/`REPZ` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REPZ               0x0000000000000800 // (1 << 11)
/**
 * @brief   The instruction accepts the `REPNE`/`REPNZ` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REPNE              0x0000000000001000 // (1 << 12)
/**
 * @brief   The instruction accepts the `REPNE`/`REPNZ` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REPNZ              0x0000000000001000 // (1 << 12)
/**
 * @brief   The instruction accepts the `BND` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_BND                0x0000000000002000 // (1 << 13)
/**
 * @brief   The instruction accepts the `XACQUIRE` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_XACQUIRE           0x0000000000004000 // (1 << 14)
/**
 * @brief   The instruction accepts the `XRELEASE` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_XRELEASE           0x0000000000008000 // (1 << 15)
/**
 * @brief   The instruction accepts the `XACQUIRE`/`XRELEASE` prefixes (`0xF2`, `0xF3`) without
 *          the `LOCK` prefix (`0x0F`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_HLE_WITHOUT_LOCK   0x0000000000010000 // (1 << 16)
/**
 * @brief   The instruction accepts branch hints (0x2E, 0x3E).
 */
#define ZYDIS_ATTRIB_ACCEPTS_BRANCH_HINTS       0x0000000000020000 // (1 << 17)
/**
 * @brief   The instruction accepts segment prefixes (`0x2E`, `0x36`, `0x3E`, `0x26`, `0x64`,
 *          `0x65`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_SEGMENT            0x0000000000040000 // (1 << 18)
/**
 * @brief   The instruction has the `LOCK` prefix (`0xF0`).
 */
#define ZYDIS_ATTRIB_HAS_LOCK                   0x0000000000080000 // (1 << 19)
/**
 * @brief   The instruction has the `REP` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_HAS_REP                    0x0000000000100000 // (1 << 20)
/**
 * @brief   The instruction has the `REPE`/`REPZ` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_HAS_REPE                   0x0000000000200000 // (1 << 21)
/**
 * @brief   The instruction has the `REPE`/`REPZ` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_HAS_REPZ                   0x0000000000200000 // (1 << 21)
/**
 * @brief   The instruction has the `REPNE`/`REPNZ` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_HAS_REPNE                  0x0000000000400000 // (1 << 22)
/**
 * @brief   The instruction has the `REPNE`/`REPNZ` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_HAS_REPNZ                  0x0000000000400000 // (1 << 22)
/**
 * @brief   The instruction has the `BND` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_HAS_BND                    0x0000000000800000 // (1 << 23)
/**
 * @brief   The instruction has the `XACQUIRE` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_HAS_XACQUIRE               0x0000000001000000 // (1 << 24)
/**
 * @brief   The instruction has the `XRELEASE` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_HAS_XRELEASE               0x0000000002000000 // (1 << 25)
/**
 * @brief   The instruction has the branch-not-taken hint (`0x2E`).
 */
#define ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN       0x0000000004000000 // (1 << 26)
/**
 * @brief   The instruction has the branch-taken hint (`0x3E`).
 */
#define ZYDIS_ATTRIB_HAS_BRANCH_TAKEN           0x0000000008000000 // (1 << 27)
/**
 * @brief   The instruction has a segment modifier.
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT                0x00000003F0000000
/**
 * @brief   The instruction has the `CS` segment modifier (`0x2E`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_CS             0x0000000010000000 // (1 << 28)
/**
 * @brief   The instruction has the `SS` segment modifier (`0x36`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_SS             0x0000000020000000 // (1 << 29)
/**
 * @brief   The instruction has the `DS` segment modifier (`0x3E`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_DS             0x0000000040000000 // (1 << 30)
/**
 * @brief   The instruction has the `ES` segment modifier (`0x26`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_ES             0x0000000080000000 // (1 << 31)
/**
 * @brief   The instruction has the `FS` segment modifier (`0x64`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_FS             0x0000000100000000 // (1 << 32)
/**
 * @brief   The instruction has the `GS` segment modifier (`0x65`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_GS             0x0000000200000000 // (1 << 33)
/**
 * @brief   The instruction has the operand-size override prefix (`0x66`).
 */
#define ZYDIS_ATTRIB_HAS_OPERANDSIZE            0x0000000400000000 // (1 << 34) // TODO: rename
/**
 * @brief   The instruction has the address-size override prefix (`0x67`).
 */
#define ZYDIS_ATTRIB_HAS_ADDRESSSIZE            0x0000000800000000 // (1 << 35) // TODO: rename

/* ---------------------------------------------------------------------------------------------- */
/* R/E/FLAGS info                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisCPUFlags` data-type.
 */
typedef ZyanU32 ZydisCPUFlags;

/**
 * @brief   Defines the `ZydisCPUFlag` enum.
 */
typedef enum ZydisCPUFlag_
{
    /**
     * @brief   Carry flag.
     */
    ZYDIS_CPUFLAG_CF,
    /**
     * @brief   Parity flag.
     */
    ZYDIS_CPUFLAG_PF,
    /**
     * @brief   Adjust flag.
     */
    ZYDIS_CPUFLAG_AF,
    /**
     * @brief   Zero flag.
     */
    ZYDIS_CPUFLAG_ZF,
    /**
     * @brief   Sign flag.
     */
    ZYDIS_CPUFLAG_SF,
    /**
     * @brief   Trap flag.
     */
    ZYDIS_CPUFLAG_TF,
    /**
     * @brief   Interrupt enable flag.
     */
    ZYDIS_CPUFLAG_IF,
    /**
     * @brief   Direction flag.
     */
    ZYDIS_CPUFLAG_DF,
    /**
     * @brief   Overflow flag.
     */
    ZYDIS_CPUFLAG_OF,
    /**
     * @brief   I/O privilege level flag.
     */
    ZYDIS_CPUFLAG_IOPL,
    /**
     * @brief   Nested task flag.
     */
    ZYDIS_CPUFLAG_NT,
    /**
     * @brief   Resume flag.
     */
    ZYDIS_CPUFLAG_RF,
    /**
     * @brief   Virtual 8086 mode flag.
     */
    ZYDIS_CPUFLAG_VM,
    /**
     * @brief   Alignment check.
     */
    ZYDIS_CPUFLAG_AC,
    /**
     * @brief   Virtual interrupt flag.
     */
    ZYDIS_CPUFLAG_VIF,
    /**
     * @brief   Virtual interrupt pending.
     */
    ZYDIS_CPUFLAG_VIP,
    /**
     * @brief   Able to use CPUID instruction.
     */
    ZYDIS_CPUFLAG_ID,
    /**
     * @brief   FPU condition-code flag 0.
     */
    ZYDIS_CPUFLAG_C0,
    /**
     * @brief   FPU condition-code flag 1.
     */
    ZYDIS_CPUFLAG_C1,
    /**
     * @brief   FPU condition-code flag 2.
     */
    ZYDIS_CPUFLAG_C2,
    /**
     * @brief   FPU condition-code flag 3.
     */
    ZYDIS_CPUFLAG_C3,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_CPUFLAG_MAX_VALUE = ZYDIS_CPUFLAG_C3,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_CPUFLAG_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_CPUFLAG_MAX_VALUE)
} ZydisCPUFlag;

/**
 * @brief   Defines the `ZydisCPUFlagAction` enum.
 */
typedef enum ZydisCPUFlagAction_
{
    /**
     * @brief   The CPU flag is not touched by the instruction.
     */
    ZYDIS_CPUFLAG_ACTION_NONE,
    /**
     * @brief   The CPU flag is tested (read).
     */
    ZYDIS_CPUFLAG_ACTION_TESTED,
    /**
     * @brief   The CPU flag is tested and modified afterwards (read-write).
     */
    ZYDIS_CPUFLAG_ACTION_TESTED_MODIFIED,
    /**
     * @brief   The CPU flag is modified (write).
     */
    ZYDIS_CPUFLAG_ACTION_MODIFIED,
    /**
     * @brief   The CPU flag is set to 0 (write).
     */
    ZYDIS_CPUFLAG_ACTION_SET_0,
    /**
     * @brief   The CPU flag is set to 1 (write).
     */
    ZYDIS_CPUFLAG_ACTION_SET_1,
    /**
     * @brief   The CPU flag is undefined (write).
     */
    ZYDIS_CPUFLAG_ACTION_UNDEFINED,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_CPUFLAG_ACTION_MAX_VALUE = ZYDIS_CPUFLAG_ACTION_UNDEFINED,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_CPUFLAG_ACTION_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_CPUFLAG_ACTION_MAX_VALUE)
} ZydisCPUFlagAction;

/* ---------------------------------------------------------------------------------------------- */
/* Branch types                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisBranchType` enum.
 */
typedef enum ZydisBranchType_
{
    /**
     * @brief   The instruction is not a branch instruction.
     */
    ZYDIS_BRANCH_TYPE_NONE,
    /**
     * @brief   The instruction is a short (8-bit) branch instruction.
     */
    ZYDIS_BRANCH_TYPE_SHORT,
    /**
     * @brief   The instruction is a near (16-bit or 32-bit) branch instruction.
     */
    ZYDIS_BRANCH_TYPE_NEAR,
    /**
     * @brief   The instruction is a far (inter-segment) branch instruction.
     */
    ZYDIS_BRANCH_TYPE_FAR,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_BRANCH_TYPE_MAX_VALUE = ZYDIS_BRANCH_TYPE_FAR,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_BRANCH_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_BRANCH_TYPE_MAX_VALUE)
} ZydisBranchType;

/* ---------------------------------------------------------------------------------------------- */
/* SSE/AVX exception-class                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisExceptionClass` enum.
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

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_EXCEPTION_CLASS_MAX_VALUE = ZYDIS_EXCEPTION_CLASS_K21,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_EXCEPTION_CLASS_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_EXCEPTION_CLASS_MAX_VALUE)
} ZydisExceptionClass;

/* ---------------------------------------------------------------------------------------------- */
/* AVX mask mode                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisMaskMode` enum.
 */
typedef enum ZydisMaskMode_
{
    ZYDIS_MASK_MODE_INVALID,
    /**
     * @brief   Masking is disabled for the current instruction (`K0` register is used).
     */
    ZYDIS_MASK_MODE_DISABLED,
    /**
     * @brief   The embedded mask register is used as a merge-mask.
     */
    ZYDIS_MASK_MODE_MERGING,
    /**
     * @brief   The embedded mask register is used as a zero-mask.
     */
    ZYDIS_MASK_MODE_ZEROING,
    /**
     * @brief   The embedded mask register is used as a control-mask (element selector).
     */
    ZYDIS_MASK_MODE_CONTROL,
    /**
     * @brief   The embedded mask register is used as a zeroing control-mask (element selector).
     */
    ZYDIS_MASK_MODE_CONTROL_ZEROING,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_MASK_MODE_MAX_VALUE = ZYDIS_MASK_MODE_CONTROL_ZEROING,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MASK_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_MASK_MODE_MAX_VALUE)
} ZydisMaskMode;

/* ---------------------------------------------------------------------------------------------- */
/* AVX broadcast-mode                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisBroadcastMode` enum.
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
     * @brief   Maximum value of this enum.
     */
    ZYDIS_BROADCAST_MODE_MAX_VALUE = ZYDIS_BROADCAST_MODE_8_TO_16,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_BROADCAST_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_BROADCAST_MODE_MAX_VALUE)
} ZydisBroadcastMode;

/* ---------------------------------------------------------------------------------------------- */
/* AVX rounding-mode                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisRoundingMode` enum.
 */
typedef enum ZydisRoundingMode_
{
    ZYDIS_ROUNDING_MODE_INVALID,
    /**
     * @brief   Round to nearest.
     */
    ZYDIS_ROUNDING_MODE_RN,
    /**
     * @brief   Round down.
     */
    ZYDIS_ROUNDING_MODE_RD,
    /**
     * @brief   Round up.
     */
    ZYDIS_ROUNDING_MODE_RU,
    /**
     * @brief   Round towards zero.
     */
    ZYDIS_ROUNDING_MODE_RZ,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_ROUNDING_MODE_MAX_VALUE = ZYDIS_ROUNDING_MODE_RZ,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_ROUNDING_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_ROUNDING_MODE_MAX_VALUE)
} ZydisRoundingMode;

/* ---------------------------------------------------------------------------------------------- */
/* KNC swizzle-mode                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisSwizzleMode` enum.
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
     * @brief   Maximum value of this enum.
     */
    ZYDIS_SWIZZLE_MODE_MAX_VALUE = ZYDIS_SWIZZLE_MODE_DDDD,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_SWIZZLE_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_SWIZZLE_MODE_MAX_VALUE)
} ZydisSwizzleMode;

/* ---------------------------------------------------------------------------------------------- */
/* KNC conversion-mode                                                                            */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisConversionMode` enum.
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
     * @brief   Maximum value of this enum.
     */
    ZYDIS_CONVERSION_MODE_MAX_VALUE = ZYDIS_CONVERSION_MODE_UINT16,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_CONVERSION_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_CONVERSION_MODE_MAX_VALUE)
} ZydisConversionMode;

/* ---------------------------------------------------------------------------------------------- */
/* Legacy prefix type                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisPrefixType` enum.
 */
typedef enum ZydisPrefixType_
{
    /**
     * @brief   The prefix is ignored by the instruction.
     *
     * This applies to all prefixes that are not accepted by the instruction in general or the
     * ones that are overwritten by a prefix of the same group closer to the instruction opcode.
     */
    ZYDIS_PREFIX_TYPE_IGNORED,
    /**
     * @brief   The prefix is effectively used by the instruction.
     */
    ZYDIS_PREFIX_TYPE_EFFECTIVE,
    /**
     * @brief   The prefix is used as a mandatory prefix.
     *
     * A mandatory prefix is interpreted as an opcode extension and has no further effect on the
     * instruction.
     */
    ZYDIS_PREFIX_TYPE_MANDATORY,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_PREFIX_TYPE_MAX_VALUE = ZYDIS_PREFIX_TYPE_MANDATORY,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_PREFIX_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_PREFIX_TYPE_MAX_VALUE)
} ZydisPrefixType;

// TODO: Check effective for 66/67 prefixes (currently defaults to EFFECTIVE)

/* ---------------------------------------------------------------------------------------------- */
/* Decoded instruction                                                                            */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisDecodedInstruction` struct.
 */
typedef struct ZydisDecodedInstruction_
{
    /**
     * @brief   The machine mode used to decode this instruction.
     */
    ZydisMachineMode machine_mode;
    /**
     * @brief   The instruction-mnemonic.
     */
    ZydisMnemonic mnemonic;
    /**
     * @brief   The length of the decoded instruction.
     */
    ZyanU8 length;
    /**
     * @brief   The instruction-encoding (`LEGACY`, `3DNOW`, `VEX`, `EVEX`, `XOP`).
     */
    ZydisInstructionEncoding encoding;
    /**
     * @brief   The opcode-map.
     */
    ZydisOpcodeMap opcode_map;
    /**
     * @brief   The instruction-opcode.
     */
    ZyanU8 opcode;
    /**
     * @brief   The stack width.
     */
    ZyanU8 stack_width;
    /**
     * @brief   The effective operand width.
     */
    ZyanU8 operand_width;
    /**
     * @brief   The effective address width.
     */
    ZyanU8 address_width;
    /**
     * @brief   The number of instruction-operands.
     */
    ZyanU8 operand_count;
    /**
     * @brief   Detailed info for all instruction operands.
     *
     * Explicit operands are guaranteed to be in the front and ordered as they are printed
     * by the formatter in Intel mode. No assumptions can be made about the order of hidden
     * operands, except that they always located behind the explicit operands.
     */
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    /**
     * @brief  Instruction attributes.
     */
    ZydisInstructionAttributes attributes;
    /**
     * @brief   Information about accessed CPU flags.
     */
    struct ZydisDecodedInstructionAccessedFlags_
    {
        /**
         * @brief   The CPU-flag action.
         *
         * Use `ZydisGetAccessedFlagsByAction` to get a mask with all flags matching a specific
         * action.
         */
        ZydisCPUFlagAction action;
    } accessed_flags[ZYDIS_CPUFLAG_MAX_VALUE + 1];
    /**
     * @brief   Extended info for `AVX` instructions.
     */
    struct ZydisDecodedInstructionAvx_
    {
        /**
         * @brief   The `AVX` vector-length.
         */
        ZyanU16 vector_length;
        /**
         * @brief   Info about the embedded writemask-register (`AVX-512` and `KNC` only).
         */
        struct ZydisDecodedInstructionAvxMask_
        {
            /**
             * @brief   The masking mode.
             */
            ZydisMaskMode mode;
            /**
             * @brief   The mask register.
             */
            ZydisRegister reg;
        } mask;
        /**
         * @brief   Contains info about the `AVX` broadcast.
         */
        struct ZydisDecodedInstructionAvxBroadcast_
        {
            /**
             * @brief   Signals, if the broadcast is a static broadcast.
             *
             * This is the case for instructions with inbuilt broadcast functionality, which is
             * always active and not controlled by the `EVEX/MVEX.RC` bits.
             */
            ZyanBool is_static;
            /**
             * @brief   The `AVX` broadcast-mode.
             */
            ZydisBroadcastMode mode;
        } broadcast;
        /**
         * @brief   Contains info about the `AVX` rounding.
         */
        struct ZydisDecodedInstructionAvxRounding_
        {
            /**
             * @brief   The `AVX` rounding-mode.
             */
            ZydisRoundingMode mode;
        } rounding;
        /**
         * @brief   Contains info about the `AVX` register-swizzle (`KNC` only).
         */
        struct ZydisDecodedInstructionAvxSwizzle_
        {
            /**
             * @brief   The `AVX` register-swizzle mode.
             */
            ZydisSwizzleMode mode;
        } swizzle;
        /**
         * @brief   Contains info about the `AVX` data-conversion (`KNC` only).
         */
        struct ZydisDecodedInstructionAvxConversion_
        {
            /**
             * @brief   The `AVX` data-conversion mode.
             */
            ZydisConversionMode mode;
        } conversion;
        /**
         * @brief   Signals, if the `SAE` (suppress-all-exceptions) functionality is enabled for
         *          the instruction.
         */
        ZyanBool has_sae;
        /**
         * @brief   Signals, if the instruction has a memory-eviction-hint (`KNC` only).
         */
        ZyanBool has_eviction_hint;
        // TODO: publish EVEX tuple-type and MVEX functionality
    } avx;
    /**
     * @brief   Meta info.
     */
    struct ZydisDecodedInstructionMeta_
    {
        /**
         * @brief   The instruction category.
         */
        ZydisInstructionCategory category;
        /**
         * @brief   The ISA-set.
         */
        ZydisISASet isa_set;
        /**
         * @brief   The ISA-set extension.
         */
        ZydisISAExt isa_ext;
        /**
         * @brief   The branch type.
         */
        ZydisBranchType branch_type;
        /**
         * @brief   The exception class.
         */
        ZydisExceptionClass exception_class;
    } meta;
    /**
     * @brief   Detailed info about different instruction-parts like `ModRM`, `SIB` or
     *          encoding-prefixes.
     */
    struct ZydisDecodedInstructionRaw_
    {
        /**
         * @brief   The number of legacy prefixes.
         */
        ZyanU8 prefix_count;
        /**
         * @brief   Detailed info about the legacy prefixes (including `REX`).
         */
        struct ZydisDecodedInstructionRawPrefixes_
        {
            /**
             * @brief   The prefix type.
             */
            ZydisPrefixType type;
            /**
             * @brief   The prefix byte.
             */
            ZyanU8 value;
        } prefixes[ZYDIS_MAX_INSTRUCTION_LENGTH];
        /**
         * @brief   Detailed info about the `REX` prefix.
         */
        struct ZydisDecodedInstructionRawRex_
        {
            /**
             * @brief   64-bit operand-size promotion.
             */
            ZyanU8 W;
            /**
             * @brief   Extension of the `ModRM.reg` field.
             */
            ZyanU8 R;
            /**
             * @brief   Extension of the `SIB.index` field.
             */
            ZyanU8 X;
            /**
             * @brief   Extension of the `ModRM.rm`, `SIB.base`, or `opcode.reg` field.
             */
            ZyanU8 B;
            /**
             * @brief   The offset of the effective `REX` byte, relative to the beginning of the
             *          instruction, in bytes.
             *
             * This offset always points to the "effective" `REX` prefix (the one closest to the
             * instruction opcode), if multiple `REX` prefixes are present.
             *
             * Note that the `REX` byte can be the first byte of the instruction, which would lead
             * to an offset of `0`. Please refer to the instruction attributes to check for the
             * presence of the `REX` prefix.
             */
            ZyanU8 offset;
        } rex;
        /**
         * @brief   Detailed info about the `XOP` prefix.
         */
        struct ZydisDecodedInstructionRawXop_
        {
            /**
             * @brief   Extension of the `ModRM.reg` field (inverted).
             */
            ZyanU8 R;
            /**
             * @brief   Extension of the `SIB.index` field (inverted).
             */
            ZyanU8 X;
            /**
             * @brief   Extension of the `ModRM.rm`, `SIB.base`, or `opcode.reg` field (inverted).
             */
            ZyanU8 B;
            /**
             * @brief   Opcode-map specifier.
             */
            ZyanU8 m_mmmm;
            /**
             * @brief   64-bit operand-size promotion or opcode-extension.
             */
            ZyanU8 W;
            /**
             * @brief   `NDS`/`NDD` (non-destructive-source/destination) register specifier
             *          (inverted).
             */
            ZyanU8 vvvv;
            /**
             * @brief   Vector-length specifier.
             */
            ZyanU8 L;
            /**
             * @brief   Compressed legacy prefix.
             */
            ZyanU8 pp;
            /**
             * @brief   The offset of the first xop byte, relative to the beginning of the
             *          instruction, in bytes.
             */
            ZyanU8 offset;
        } xop;
        /**
         * @brief   Detailed info about the `VEX` prefix.
         */
        struct ZydisDecodedInstructionRawVex_
        {
            /**
             * @brief   Extension of the `ModRM.reg` field (inverted).
             */
            ZyanU8 R;
            /**
             * @brief   Extension of the `SIB.index` field (inverted).
             */
            ZyanU8 X;
            /**
             * @brief   Extension of the `ModRM.rm`, `SIB.base`, or `opcode.reg` field (inverted).
             */
            ZyanU8 B;
            /**
             * @brief   Opcode-map specifier.
             */
            ZyanU8 m_mmmm;
            /**
             * @brief   64-bit operand-size promotion or opcode-extension.
             */
            ZyanU8 W;
            /**
             * @brief   `NDS`/`NDD` (non-destructive-source/destination) register specifier
             *          (inverted).
             */
            ZyanU8 vvvv;
            /**
             * @brief   Vector-length specifier.
             */
            ZyanU8 L;
            /**
             * @brief   Compressed legacy prefix.
             */
            ZyanU8 pp;
            /**
             * @brief   The offset of the first `VEX` byte, relative to the beginning of the
             *          instruction, in bytes.
             */
            ZyanU8 offset;
            /**
             * @brief   The size of the `VEX` prefix, in bytes.
             */
            ZyanU8 size;
        } vex;
        /**
         * @brief   Detailed info about the `EVEX` prefix.
         */
        struct ZydisDecodedInstructionRawEvex_
        {
            /**
             * @brief   Extension of the `ModRM.reg` field (inverted).
             */
            ZyanU8 R;
            /**
             * @brief   Extension of the `SIB.index/vidx` field (inverted).
             */
            ZyanU8 X;
            /**
             * @brief   Extension of the `ModRM.rm` or `SIB.base` field (inverted).
             */
            ZyanU8 B;
            /**
             * @brief   High-16 register specifier modifier (inverted).
             */
            ZyanU8 R2;
            /**
             * @brief   Opcode-map specifier.
             */
            ZyanU8 mm;
            /**
             * @brief   64-bit operand-size promotion or opcode-extension.
             */
            ZyanU8 W;
            /**
             * @brief   `NDS`/`NDD` (non-destructive-source/destination) register specifier
             *          (inverted).
             */
            ZyanU8 vvvv;
            /**
             * @brief   Compressed legacy prefix.
             */
            ZyanU8 pp;
            /**
             * @brief   Zeroing/Merging.
             */
            ZyanU8 z;
            /**
             * @brief   Vector-length specifier or rounding-control (most significant bit).
             */
            ZyanU8 L2;
            /**
             * @brief   Vector-length specifier or rounding-control (least significant bit).
             */
            ZyanU8 L;
            /**
             * @brief   Broadcast/RC/SAE context.
             */
            ZyanU8 b;
            /**
             * @brief   High-16 `NDS`/`VIDX` register specifier.
             */
            ZyanU8 V2;
            /**
             * @brief   Embedded opmask register specifier.
             */
            ZyanU8 aaa;
            /**
             * @brief   The offset of the first evex byte, relative to the beginning of the
             *          instruction, in bytes.
             */
            ZyanU8 offset;
        } evex;
        /**
        * @brief    Detailed info about the `MVEX` prefix.
        */
        struct ZydisDecodedInstructionRawMvex_
        {
            /**
             * @brief   Extension of the `ModRM.reg` field (inverted).
             */
            ZyanU8 R;
            /**
             * @brief   Extension of the `SIB.index/vidx` field (inverted).
             */
            ZyanU8 X;
            /**
             * @brief   Extension of the `ModRM.rm` or `SIB.base` field (inverted).
             */
            ZyanU8 B;
            /**
             * @brief   High-16 register specifier modifier (inverted).
             */
            ZyanU8 R2;
            /**
             * @brief   Opcode-map specifier.
             */
            ZyanU8 mmmm;
            /**
             * @brief   64-bit operand-size promotion or opcode-extension.
             */
            ZyanU8 W;
            /**
             * @brief   `NDS`/`NDD` (non-destructive-source/destination) register specifier
             *          (inverted).
             */
            ZyanU8 vvvv;
            /**
             * @brief   Compressed legacy prefix.
             */
            ZyanU8 pp;
            /**
             * @brief   Non-temporal/eviction hint.
             */
            ZyanU8 E;
            /**
             * @brief   Swizzle/broadcast/up-convert/down-convert/static-rounding controls.
             */
            ZyanU8 SSS;
            /**
             * @brief   High-16 `NDS`/`VIDX` register specifier.
             */
            ZyanU8 V2;
            /**
             * @brief   Embedded opmask register specifier.
             */
            ZyanU8 kkk;
            /**
             * @brief   The offset of the first mvex byte, relative to the beginning of the
             *          instruction, in bytes.
             */
            ZyanU8 offset;
        } mvex;
        /**
         * @brief   Detailed info about the `ModRM` byte.
         */
        struct ZydisDecodedInstructionModRm_
        {
            /**
             * @brief   The addressing mode.
             */
            ZyanU8 mod;
            /**
             * @brief   Register specifier or opcode-extension.
             */
            ZyanU8 reg;
            /**
             * @brief   Register specifier or opcode-extension.
             */
            ZyanU8 rm;
            /**
             * @brief   The offset of the `ModRM` byte, relative to the beginning of the
             *          instruction, in bytes.
             */
            ZyanU8 offset;
        } modrm;
        /**
         * @brief   Detailed info about the `SIB` byte.
         */
        struct ZydisDecodedInstructionRawSib_
        {
            /**
             * @brief   The scale factor.
             */
            ZyanU8 scale;
            /**
             * @brief   The index-register specifier.
             */
            ZyanU8 index;
            /**
             * @brief   The base-register specifier.
             */
            ZyanU8 base;
            /**
             * @brief   The offset of the `SIB` byte, relative to the beginning of the instruction,
             *          in bytes.
             */
            ZyanU8 offset;
        } sib;
        /**
         * @brief   Detailed info about displacement-bytes.
         */
        struct ZydisDecodedInstructionRawDisp_
        {
            /**
             * @brief   The displacement value
             */
            ZyanI64 value;
            /**
             * @brief   The physical displacement size, in bits.
             */
            ZyanU8 size;
            // TODO: publish cd8 scale
            /**
             * @brief   The offset of the displacement data, relative to the beginning of the
             *          instruction, in bytes.
             */
            ZyanU8 offset;
        } disp;
        /**
         * @brief   Detailed info about immediate-bytes.
         */
        struct ZydisDecodedInstructionRawImm_
        {
            /**
             * @brief   Signals, if the immediate value is signed.
             */
            ZyanBool is_signed;
            /**
             * @brief   Signals, if the immediate value contains a relative offset. You can use
             *          `ZydisCalcAbsoluteAddress` to determine the absolute address value.
             */
            ZyanBool is_relative;
            /**
             * @brief   The immediate value.
             */
            union ZydisDecodedInstructionRawImmValue_
            {
                ZyanU64 u;
                ZyanI64 s;
            } value;
            /**
             * @brief   The physical immediate size, in bits.
             */
            ZyanU8 size;
            /**
             * @brief   The offset of the immediate data, relative to the beginning of the
             *          instruction, in bytes.
             */
            ZyanU8 offset;
        } imm[2];
    } raw;
} ZydisDecodedInstruction;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_INSTRUCTIONINFO_H */
