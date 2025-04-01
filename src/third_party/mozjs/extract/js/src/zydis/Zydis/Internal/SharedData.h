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

#ifndef ZYDIS_INTERNAL_SHAREDDATA_H
#define ZYDIS_INTERNAL_SHAREDDATA_H

#include "zydis/Zycore/Defines.h"
#include "zydis/Zydis/Mnemonic.h"
#include "zydis/Zydis/Register.h"
#include "zydis/Zydis/SharedTypes.h"
#include "zydis/Zydis/DecoderTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

// MSVC does not like types other than (un-)signed int for bit-fields
#ifdef ZYAN_MSVC
#   pragma warning(push)
#   pragma warning(disable:4214)
#endif

#pragma pack(push, 1)

/* ---------------------------------------------------------------------------------------------- */
/* Operand definition                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisSemanticOperandType` enum.
 */
typedef enum ZydisSemanticOperandType_
{
    ZYDIS_SEMANTIC_OPTYPE_UNUSED,
    ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_REG,
    ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_MEM,
    ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_IMM1,
    ZYDIS_SEMANTIC_OPTYPE_GPR8,
    ZYDIS_SEMANTIC_OPTYPE_GPR16,
    ZYDIS_SEMANTIC_OPTYPE_GPR32,
    ZYDIS_SEMANTIC_OPTYPE_GPR64,
    ZYDIS_SEMANTIC_OPTYPE_GPR16_32_64,
    ZYDIS_SEMANTIC_OPTYPE_GPR32_32_64,
    ZYDIS_SEMANTIC_OPTYPE_GPR16_32_32,
    ZYDIS_SEMANTIC_OPTYPE_GPR_ASZ,
    ZYDIS_SEMANTIC_OPTYPE_FPR,
    ZYDIS_SEMANTIC_OPTYPE_MMX,
    ZYDIS_SEMANTIC_OPTYPE_XMM,
    ZYDIS_SEMANTIC_OPTYPE_YMM,
    ZYDIS_SEMANTIC_OPTYPE_ZMM,
    ZYDIS_SEMANTIC_OPTYPE_TMM,
    ZYDIS_SEMANTIC_OPTYPE_BND,
    ZYDIS_SEMANTIC_OPTYPE_SREG,
    ZYDIS_SEMANTIC_OPTYPE_CR,
    ZYDIS_SEMANTIC_OPTYPE_DR,
    ZYDIS_SEMANTIC_OPTYPE_MASK,
    ZYDIS_SEMANTIC_OPTYPE_MEM,
    ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBX,
    ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBY,
    ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBZ,
    ZYDIS_SEMANTIC_OPTYPE_IMM,
    ZYDIS_SEMANTIC_OPTYPE_REL,
    ZYDIS_SEMANTIC_OPTYPE_PTR,
    ZYDIS_SEMANTIC_OPTYPE_AGEN,
    ZYDIS_SEMANTIC_OPTYPE_MOFFS,
    ZYDIS_SEMANTIC_OPTYPE_MIB,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_SEMANTIC_OPTYPE_MAX_VALUE = ZYDIS_SEMANTIC_OPTYPE_MIB,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_SEMANTIC_OPTYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_SEMANTIC_OPTYPE_MAX_VALUE)
} ZydisSemanticOperandType;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisInternalElementType` enum.
 */
typedef enum ZydisInternalElementType_
{
    ZYDIS_IELEMENT_TYPE_INVALID,
    ZYDIS_IELEMENT_TYPE_VARIABLE,
    ZYDIS_IELEMENT_TYPE_STRUCT,
    ZYDIS_IELEMENT_TYPE_INT,
    ZYDIS_IELEMENT_TYPE_UINT,
    ZYDIS_IELEMENT_TYPE_INT1,
    ZYDIS_IELEMENT_TYPE_INT8,
    ZYDIS_IELEMENT_TYPE_INT8X4,
    ZYDIS_IELEMENT_TYPE_INT16,
    ZYDIS_IELEMENT_TYPE_INT16X2,
    ZYDIS_IELEMENT_TYPE_INT32,
    ZYDIS_IELEMENT_TYPE_INT64,
    ZYDIS_IELEMENT_TYPE_UINT8,
    ZYDIS_IELEMENT_TYPE_UINT8X4,
    ZYDIS_IELEMENT_TYPE_UINT16,
    ZYDIS_IELEMENT_TYPE_UINT16X2,
    ZYDIS_IELEMENT_TYPE_UINT32,
    ZYDIS_IELEMENT_TYPE_UINT64,
    ZYDIS_IELEMENT_TYPE_UINT128,
    ZYDIS_IELEMENT_TYPE_UINT256,
    ZYDIS_IELEMENT_TYPE_FLOAT16,
    ZYDIS_IELEMENT_TYPE_FLOAT16X2,
    ZYDIS_IELEMENT_TYPE_FLOAT32,
    ZYDIS_IELEMENT_TYPE_FLOAT64,
    ZYDIS_IELEMENT_TYPE_FLOAT80,
    ZYDIS_IELEMENT_TYPE_BFLOAT16X2,
    ZYDIS_IELEMENT_TYPE_BCD80,
    ZYDIS_IELEMENT_TYPE_CC3,
    ZYDIS_IELEMENT_TYPE_CC5,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_IELEMENT_TYPE_MAX_VALUE = ZYDIS_IELEMENT_TYPE_CC5,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_IELEMENT_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_IELEMENT_TYPE_MAX_VALUE)
} ZydisInternalElementType;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisImplicitRegisterType` enum.
 */
typedef enum ZydisImplicitRegisterType_
{
    // TODO: Rename OSZ|ASZ|SSZ_
    ZYDIS_IMPLREG_TYPE_STATIC,
    ZYDIS_IMPLREG_TYPE_GPR_OSZ,
    ZYDIS_IMPLREG_TYPE_GPR_ASZ,
    ZYDIS_IMPLREG_TYPE_IP_ASZ,
    ZYDIS_IMPLREG_TYPE_IP_SSZ,
    ZYDIS_IMPLREG_TYPE_GPR_SSZ,
    ZYDIS_IMPLREG_TYPE_FLAGS_SSZ,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_IMPLREG_TYPE_MAX_VALUE = ZYDIS_IMPLREG_TYPE_FLAGS_SSZ,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_IMPLREG_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_IMPLREG_TYPE_MAX_VALUE)
} ZydisImplicitRegisterType;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisImplicitMemBase` enum.
 */
typedef enum ZydisImplicitMemBase_
{
    // TODO: Rename OSZ|ASZ|SSZ_
    ZYDIS_IMPLMEM_BASE_AGPR_REG,
    ZYDIS_IMPLMEM_BASE_AGPR_RM,
    ZYDIS_IMPLMEM_BASE_AAX,
    ZYDIS_IMPLMEM_BASE_ADX,
    ZYDIS_IMPLMEM_BASE_ABX,
    ZYDIS_IMPLMEM_BASE_ASI,
    ZYDIS_IMPLMEM_BASE_ADI,
    ZYDIS_IMPLMEM_BASE_SSP,
    ZYDIS_IMPLMEM_BASE_SBP,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_IMPLMEM_BASE_MAX_VALUE = ZYDIS_IMPLMEM_BASE_SBP,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_IMPLMEM_BASE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_IMPLMEM_BASE_MAX_VALUE)
} ZydisImplicitMemBase;

/* ---------------------------------------------------------------------------------------------- */

// MSVC does not correctly execute the `pragma pack(1)` compiler-directive, if we use the correct
// enum types
ZYAN_STATIC_ASSERT(ZYDIS_SEMANTIC_OPTYPE_REQUIRED_BITS     <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_OPERAND_VISIBILITY_REQUIRED_BITS  <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_OPERAND_ACTION_REQUIRED_BITS      <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_IELEMENT_TYPE_REQUIRED_BITS       <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_OPERAND_ENCODING_REQUIRED_BITS    <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_IMPLREG_TYPE_REQUIRED_BITS        <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_REGISTER_REQUIRED_BITS            <= 16);
ZYAN_STATIC_ASSERT(ZYDIS_IMPLMEM_BASE_REQUIRED_BITS        <=  8);

/**
 * Defines the `ZydisOperandDefinition` struct.
 */
typedef struct ZydisOperandDefinition_
{
    ZyanU8 type                            ZYAN_BITFIELD(ZYDIS_SEMANTIC_OPTYPE_REQUIRED_BITS);
    ZyanU8 visibility                      ZYAN_BITFIELD(ZYDIS_OPERAND_VISIBILITY_REQUIRED_BITS);
    ZyanU8 actions                         ZYAN_BITFIELD(ZYDIS_OPERAND_ACTION_REQUIRED_BITS);
    ZyanU16 size[3];
    ZyanU8 element_type                    ZYAN_BITFIELD(ZYDIS_IELEMENT_TYPE_REQUIRED_BITS);
    union
    {
        ZyanU8 encoding                    ZYAN_BITFIELD(ZYDIS_OPERAND_ENCODING_REQUIRED_BITS);
        struct
        {
            ZyanU8 type                    ZYAN_BITFIELD(ZYDIS_IMPLREG_TYPE_REQUIRED_BITS);
            union
            {
                ZyanU16 reg                ZYAN_BITFIELD(ZYDIS_REGISTER_REQUIRED_BITS);
                ZyanU8 id                  ZYAN_BITFIELD(6);
            } reg;
        } reg;
        struct
        {
            ZyanU8 seg                     ZYAN_BITFIELD(3);
            ZyanU8 base                    ZYAN_BITFIELD(ZYDIS_IMPLMEM_BASE_REQUIRED_BITS);
        } mem;
    } op;
    ZyanBool is_multisource4               ZYAN_BITFIELD(1);
    ZyanBool ignore_seg_override           ZYAN_BITFIELD(1);
} ZydisOperandDefinition;

/* ---------------------------------------------------------------------------------------------- */
/* Instruction definition                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisReadWriteAction` enum.
 */
typedef enum ZydisReadWriteAction_
{
    ZYDIS_RW_ACTION_NONE,
    ZYDIS_RW_ACTION_READ,
    ZYDIS_RW_ACTION_WRITE,
    ZYDIS_RW_ACTION_READWRITE,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_RW_ACTION_MAX_VALUE = ZYDIS_RW_ACTION_READWRITE,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_RW_ACTION_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_RW_ACTION_MAX_VALUE)
} ZydisReadWriteAction;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisInternalVectorLength` enum.
 */
typedef enum ZydisInternalVectorLength_
{
    ZYDIS_IVECTOR_LENGTH_DEFAULT,
    ZYDIS_IVECTOR_LENGTH_FIXED_128,
    ZYDIS_IVECTOR_LENGTH_FIXED_256,
    ZYDIS_IVECTOR_LENGTH_FIXED_512,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_IVECTOR_LENGTH_MAX_VALUE = ZYDIS_IVECTOR_LENGTH_FIXED_512,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_IVECTOR_LENGTH_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_IVECTOR_LENGTH_MAX_VALUE)
} ZydisInternalVectorLength;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisInternalElementSize` enum.
 */
typedef enum ZydisInternalElementSize_
{
    ZYDIS_IELEMENT_SIZE_INVALID,
    ZYDIS_IELEMENT_SIZE_8,
    ZYDIS_IELEMENT_SIZE_16,
    ZYDIS_IELEMENT_SIZE_32,
    ZYDIS_IELEMENT_SIZE_64,
    ZYDIS_IELEMENT_SIZE_128,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_IELEMENT_SIZE_MAX_VALUE = ZYDIS_IELEMENT_SIZE_128,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_IELEMENT_SIZE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_IELEMENT_SIZE_MAX_VALUE)
} ZydisInternalElementSize;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisEVEXFunctionality` enum.
 */
typedef enum ZydisEVEXFunctionality_
{
    ZYDIS_EVEX_FUNC_INVALID,
    /**
     * `EVEX.b` enables broadcast functionality.
     */
    ZYDIS_EVEX_FUNC_BC,
    /**
     * `EVEX.b` enables embedded-rounding functionality.
     */
    ZYDIS_EVEX_FUNC_RC,
    /**
     * `EVEX.b` enables sae functionality.
     */
    ZYDIS_EVEX_FUNC_SAE,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_EVEX_FUNC_MAX_VALUE = ZYDIS_EVEX_FUNC_SAE,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_EVEX_FUNC_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_EVEX_FUNC_MAX_VALUE)
} ZydisEVEXFunctionality;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisEVEXTupleType` enum.
 */
typedef enum ZydisEVEXTupleType_
{
    ZYDIS_TUPLETYPE_INVALID,
    /**
     * Full Vector
     */
    ZYDIS_TUPLETYPE_FV,
    /**
     * Half Vector
     */
    ZYDIS_TUPLETYPE_HV,
    /**
     * Full Vector Mem
     */
    ZYDIS_TUPLETYPE_FVM,
    /**
     * Tuple1 Scalar
     */
    ZYDIS_TUPLETYPE_T1S,
    /**
     * Tuple1 Fixed
     */
    ZYDIS_TUPLETYPE_T1F,
    /**
     * Tuple1 4x32
     */
    ZYDIS_TUPLETYPE_T1_4X,
    /**
     * Gather / Scatter
     */
    ZYDIS_TUPLETYPE_GSCAT,
    /**
     * Tuple2
     */
    ZYDIS_TUPLETYPE_T2,
    /**
     * Tuple4
     */
    ZYDIS_TUPLETYPE_T4,
    /**
     * Tuple8
     */
    ZYDIS_TUPLETYPE_T8,
    /**
     * Half Mem
     */
    ZYDIS_TUPLETYPE_HVM,
    /**
     * QuarterMem
     */
    ZYDIS_TUPLETYPE_QVM,
    /**
     * OctMem
     */
    ZYDIS_TUPLETYPE_OVM,
    /**
     * Mem128
     */
    ZYDIS_TUPLETYPE_M128,
    /**
     * MOVDDUP
     */
    ZYDIS_TUPLETYPE_DUP,
    /**
     * Quarter of the vector-length.
     */
    ZYDIS_TUPLETYPE_QUARTER,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_TUPLETYPE_MAX_VALUE = ZYDIS_TUPLETYPE_QUARTER,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_TUPLETYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_TUPLETYPE_MAX_VALUE)
} ZydisEVEXTupleType;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisMVEXFunctionality` enum.
 */
typedef enum ZydisMVEXFunctionality_
{
    /**
     * The `MVEX.SSS` value is ignored.
     */
    ZYDIS_MVEX_FUNC_IGNORED,
    /**
     * `MVEX.SSS` must be `000b`.
     */
    ZYDIS_MVEX_FUNC_INVALID,
    /**
     * `MVEX.SSS` controls embedded-rounding functionality.
     */
    ZYDIS_MVEX_FUNC_RC,
    /**
     * `MVEX.SSS` controls sae functionality.
     */
    ZYDIS_MVEX_FUNC_SAE,
    /**
     * No special operation (32bit float elements).
     */
    ZYDIS_MVEX_FUNC_F_32,
    /**
     * No special operation (32bit uint elements).
     */
    ZYDIS_MVEX_FUNC_I_32,
    /**
     * No special operation (64bit float elements).
     */
    ZYDIS_MVEX_FUNC_F_64,
    /**
     * No special operation (64bit uint elements).
     */
    ZYDIS_MVEX_FUNC_I_64,
    /**
     * Sf32(reg) or Si32(reg).
     */
    ZYDIS_MVEX_FUNC_SWIZZLE_32,
    /**
     * Sf64(reg) or Si64(reg).
     */
    ZYDIS_MVEX_FUNC_SWIZZLE_64,
    /**
     * Sf32(mem).
     */
    ZYDIS_MVEX_FUNC_SF_32,
    /**
     * Sf32(mem) broadcast only.
     */
    ZYDIS_MVEX_FUNC_SF_32_BCST,
    /**
     * Sf32(mem) broadcast 4to16 only.
     */
    ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16,
    /**
     * Sf64(mem).
     */
    ZYDIS_MVEX_FUNC_SF_64,
    /**
     * Si32(mem).
     */
    ZYDIS_MVEX_FUNC_SI_32,
    /**
     * Si32(mem) broadcast only.
     */
    ZYDIS_MVEX_FUNC_SI_32_BCST,
    /**
     * Si32(mem) broadcast 4to16 only.
     */
    ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16,
    /**
     * Si64(mem).
     */
    ZYDIS_MVEX_FUNC_SI_64,
    /**
     * Uf32.
     */
    ZYDIS_MVEX_FUNC_UF_32,
    /**
     * Uf64.
     */
    ZYDIS_MVEX_FUNC_UF_64,
    /**
     * Ui32.
     */
    ZYDIS_MVEX_FUNC_UI_32,
    /**
     * Ui64.
     */
    ZYDIS_MVEX_FUNC_UI_64,
    /**
     * Df32.
     */
    ZYDIS_MVEX_FUNC_DF_32,
    /**
     * Df64.
     */
    ZYDIS_MVEX_FUNC_DF_64,
    /**
     * Di32.
     */
    ZYDIS_MVEX_FUNC_DI_32,
    /**
     * Di64.
     */
    ZYDIS_MVEX_FUNC_DI_64,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_MVEX_FUNC_MAX_VALUE = ZYDIS_MVEX_FUNC_DI_64,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MVEX_FUNC_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_MVEX_FUNC_MAX_VALUE)
} ZydisMVEXFunctionality;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisVEXStaticBroadcast` enum.
 */
typedef enum ZydisVEXStaticBroadcast
{
    ZYDIS_VEX_STATIC_BROADCAST_NONE,
    ZYDIS_VEX_STATIC_BROADCAST_1_TO_2,
    ZYDIS_VEX_STATIC_BROADCAST_1_TO_4,
    ZYDIS_VEX_STATIC_BROADCAST_1_TO_8,
    ZYDIS_VEX_STATIC_BROADCAST_1_TO_16,
    ZYDIS_VEX_STATIC_BROADCAST_1_TO_32,
    ZYDIS_VEX_STATIC_BROADCAST_2_TO_4,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_VEX_STATIC_BROADCAST_MAX_VALUE = ZYDIS_VEX_STATIC_BROADCAST_2_TO_4,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_VEX_STATIC_BROADCAST_REQUIRED_BITS =
        ZYAN_BITS_TO_REPRESENT(ZYDIS_VEX_STATIC_BROADCAST_MAX_VALUE)
} ZydisVEXStaticBroadcast;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisEVEXStaticBroadcast` enum.
 */
typedef enum ZydisEVEXStaticBroadcast_
{
    ZYDIS_EVEX_STATIC_BROADCAST_NONE,
    ZYDIS_EVEX_STATIC_BROADCAST_1_TO_2,
    ZYDIS_EVEX_STATIC_BROADCAST_1_TO_4,
    ZYDIS_EVEX_STATIC_BROADCAST_1_TO_8,
    ZYDIS_EVEX_STATIC_BROADCAST_1_TO_16,
    ZYDIS_EVEX_STATIC_BROADCAST_1_TO_32,
    ZYDIS_EVEX_STATIC_BROADCAST_1_TO_64,
    ZYDIS_EVEX_STATIC_BROADCAST_2_TO_4,
    ZYDIS_EVEX_STATIC_BROADCAST_2_TO_8,
    ZYDIS_EVEX_STATIC_BROADCAST_2_TO_16,
    ZYDIS_EVEX_STATIC_BROADCAST_4_TO_8,
    ZYDIS_EVEX_STATIC_BROADCAST_4_TO_16,
    ZYDIS_EVEX_STATIC_BROADCAST_8_TO_16,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_EVEX_STATIC_BROADCAST_MAX_VALUE = ZYDIS_EVEX_STATIC_BROADCAST_8_TO_16,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_EVEX_STATIC_BROADCAST_REQUIRED_BITS =
        ZYAN_BITS_TO_REPRESENT(ZYDIS_EVEX_STATIC_BROADCAST_MAX_VALUE)
} ZydisEVEXStaticBroadcast;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisMVEXStaticBroadcast` enum.
 */
typedef enum ZydisMVEXStaticBroadcast_
{
    ZYDIS_MVEX_STATIC_BROADCAST_NONE,
    ZYDIS_MVEX_STATIC_BROADCAST_1_TO_8,
    ZYDIS_MVEX_STATIC_BROADCAST_1_TO_16,
    ZYDIS_MVEX_STATIC_BROADCAST_4_TO_8,
    ZYDIS_MVEX_STATIC_BROADCAST_4_TO_16,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_MVEX_STATIC_BROADCAST_MAX_VALUE = ZYDIS_MVEX_STATIC_BROADCAST_4_TO_16,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MVEX_STATIC_BROADCAST_REQUIRED_BITS =
        ZYAN_BITS_TO_REPRESENT(ZYDIS_MVEX_STATIC_BROADCAST_MAX_VALUE)
} ZydisMVEXStaticBroadcast;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisMaskPolicy` enum.
 */
typedef enum ZydisMaskPolicy_
{
    ZYDIS_MASK_POLICY_INVALID,
    /**
     * The instruction accepts mask-registers other than the default-mask (K0), but
     *          does not require them.
     */
    ZYDIS_MASK_POLICY_ALLOWED,
    /**
     * The instruction requires a mask-register other than the default-mask (K0).
     */
    ZYDIS_MASK_POLICY_REQUIRED,
    /**
     * The instruction does not allow a mask-register other than the default-mask (K0).
     */
    ZYDIS_MASK_POLICY_FORBIDDEN,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_MASK_POLICY_MAX_VALUE = ZYDIS_MASK_POLICY_FORBIDDEN,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MASK_POLICY_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_MASK_POLICY_MAX_VALUE)
} ZydisMaskPolicy;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisMaskOverride` enum.
 */
typedef enum ZydisMaskOverride_
{
    ZYDIS_MASK_OVERRIDE_DEFAULT,
    ZYDIS_MASK_OVERRIDE_ZEROING,
    ZYDIS_MASK_OVERRIDE_CONTROL,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_MASK_OVERRIDE_MAX_VALUE = ZYDIS_MASK_OVERRIDE_CONTROL,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MASK_OVERRIDE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_MASK_OVERRIDE_MAX_VALUE)
} ZydisMaskOverride;

/* ---------------------------------------------------------------------------------------------- */

#define ZYDIS_OPDEF_REQUIRED_BITS \
    ZYAN_MAX(ZYDIS_REGKIND_REQUIRED_BITS, ZYDIS_MEMOP_TYPE_REQUIRED_BITS + 1) + 1

#define ZYDIS_OPDEF_GET_REG(operand_definition) \
    ((operand_definition) & ((1 << ZYDIS_REGKIND_REQUIRED_BITS   ) - 1))

#define ZYDIS_OPDEF_GET_MEM(operand_definition) \
    ((operand_definition) & ((1 << ZYDIS_MEMOP_TYPE_REQUIRED_BITS) - 1))

#define ZYDIS_OPDEF_GET_REG_HIGH_BIT(operand_definition) \
    (((operand_definition) >> ZYDIS_REGKIND_REQUIRED_BITS   ) & 0x01)

#define ZYDIS_OPDEF_GET_MEM_HIGH_BIT(operand_definition) \
    (((operand_definition) >> ZYDIS_MEMOP_TYPE_REQUIRED_BITS) & 0x01)

// MSVC does not correctly execute the `pragma pack(1)` compiler-directive, if we use the correct
// enum types
ZYAN_STATIC_ASSERT(ZYDIS_MNEMONIC_REQUIRED_BITS        <= 16);
ZYAN_STATIC_ASSERT(ZYDIS_CATEGORY_REQUIRED_BITS        <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_ISA_SET_REQUIRED_BITS         <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_ISA_EXT_REQUIRED_BITS         <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_BRANCH_TYPE_REQUIRED_BITS     <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_EXCEPTION_CLASS_REQUIRED_BITS <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_OPDEF_REQUIRED_BITS           <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_RW_ACTION_REQUIRED_BITS       <=  8);

#ifndef ZYDIS_MINIMAL_MODE
#   define ZYDIS_INSTRUCTION_DEFINITION_BASE \
        ZyanU16 mnemonic                       ZYAN_BITFIELD(ZYDIS_MNEMONIC_REQUIRED_BITS); \
        ZyanU8 operand_count                   ZYAN_BITFIELD( 4); \
        ZyanU8 operand_count_visible           ZYAN_BITFIELD( 3); \
        ZyanU16 operand_reference              ZYAN_BITFIELD(15); \
        ZyanU8 operand_size_map                ZYAN_BITFIELD( 3); \
        ZyanU8 address_size_map                ZYAN_BITFIELD( 2); \
        ZyanU8 flags_reference                 ZYAN_BITFIELD( 7); \
        ZyanBool requires_protected_mode       ZYAN_BITFIELD( 1); \
        ZyanBool no_compat_mode                ZYAN_BITFIELD( 1); \
        ZyanU8 category                        ZYAN_BITFIELD(ZYDIS_CATEGORY_REQUIRED_BITS); \
        ZyanU8 isa_set                         ZYAN_BITFIELD(ZYDIS_ISA_SET_REQUIRED_BITS); \
        ZyanU8 isa_ext                         ZYAN_BITFIELD(ZYDIS_ISA_EXT_REQUIRED_BITS); \
        ZyanU8 branch_type                     ZYAN_BITFIELD(ZYDIS_BRANCH_TYPE_REQUIRED_BITS); \
        ZyanU8 exception_class                 ZYAN_BITFIELD(ZYDIS_EXCEPTION_CLASS_REQUIRED_BITS); \
        ZyanU8 op_reg                          ZYAN_BITFIELD(ZYDIS_OPDEF_REQUIRED_BITS); \
        ZyanU8 op_rm                           ZYAN_BITFIELD(ZYDIS_OPDEF_REQUIRED_BITS); \
        ZyanU8 cpu_state                       ZYAN_BITFIELD(ZYDIS_RW_ACTION_REQUIRED_BITS); \
        ZyanU8 fpu_state                       ZYAN_BITFIELD(ZYDIS_RW_ACTION_REQUIRED_BITS); \
        ZyanU8 xmm_state                       ZYAN_BITFIELD(ZYDIS_RW_ACTION_REQUIRED_BITS); \
        ZyanBool accepts_segment               ZYAN_BITFIELD( 1)
#else
#   define ZYDIS_INSTRUCTION_DEFINITION_BASE \
        ZyanU16 mnemonic                       ZYAN_BITFIELD(ZYDIS_MNEMONIC_REQUIRED_BITS); \
        ZyanU8 operand_size_map                ZYAN_BITFIELD( 3); \
        ZyanU8 address_size_map                ZYAN_BITFIELD( 2); \
        ZyanBool requires_protected_mode       ZYAN_BITFIELD( 1); \
        ZyanBool no_compat_mode                ZYAN_BITFIELD( 1); \
        ZyanU8 op_reg                          ZYAN_BITFIELD(ZYDIS_OPDEF_REQUIRED_BITS); \
        ZyanU8 op_rm                           ZYAN_BITFIELD(ZYDIS_OPDEF_REQUIRED_BITS)
#endif

#define ZYDIS_INSTRUCTION_DEFINITION_BASE_VECTOR \
    ZYDIS_INSTRUCTION_DEFINITION_BASE; \
    ZyanU8 op_ndsndd                       ZYAN_BITFIELD(ZYDIS_OPDEF_REQUIRED_BITS)

#define ZYDIS_INSTRUCTION_DEFINITION_BASE_VECTOR_INTEL \
    ZYDIS_INSTRUCTION_DEFINITION_BASE_VECTOR; \
    ZyanBool is_gather                     ZYAN_BITFIELD( 1); \
    ZyanBool no_source_dest_match          ZYAN_BITFIELD( 1); \
    ZyanBool no_source_source_match        ZYAN_BITFIELD( 1)        // TODO: Could be moved to VEX

/**
 * Defines the `ZydisInstructionDefinition` struct.
 */
typedef struct ZydisInstructionDefinition_
{
    ZYDIS_INSTRUCTION_DEFINITION_BASE;
} ZydisInstructionDefinition;

/**
 * Defines the `ZydisInstructionDefinitionLEGACY` struct.
 */
typedef struct ZydisInstructionDefinitionLEGACY_
{
    ZYDIS_INSTRUCTION_DEFINITION_BASE;
#ifndef ZYDIS_MINIMAL_MODE
    ZyanBool is_privileged                 ZYAN_BITFIELD( 1);
#endif
    ZyanBool accepts_LOCK                  ZYAN_BITFIELD( 1);
#ifndef ZYDIS_MINIMAL_MODE
    ZyanBool accepts_REP                   ZYAN_BITFIELD( 1);
    ZyanBool accepts_REPEREPZ              ZYAN_BITFIELD( 1);
    ZyanBool accepts_REPNEREPNZ            ZYAN_BITFIELD( 1);
    ZyanBool accepts_BOUND                 ZYAN_BITFIELD( 1);
    ZyanBool accepts_XACQUIRE              ZYAN_BITFIELD( 1);
    ZyanBool accepts_XRELEASE              ZYAN_BITFIELD( 1);
    ZyanBool accepts_NOTRACK               ZYAN_BITFIELD( 1);
    ZyanBool accepts_hle_without_lock      ZYAN_BITFIELD( 1);
    ZyanBool accepts_branch_hints          ZYAN_BITFIELD( 1);
#endif
} ZydisInstructionDefinitionLEGACY;

/**
 * Defines the `ZydisInstructionDefinition3DNOW` struct.
 */
typedef struct ZydisInstructionDefinition3DNOW_
{
    ZYDIS_INSTRUCTION_DEFINITION_BASE;
} ZydisInstructionDefinition3DNOW;

/**
 * Defines the `ZydisInstructionDefinitionXOP` struct.
 */
typedef struct ZydisInstructionDefinitionXOP_
{
    ZYDIS_INSTRUCTION_DEFINITION_BASE_VECTOR;
} ZydisInstructionDefinitionXOP;

// MSVC does not correctly execute the `pragma pack(1)` compiler-directive, if we use the correct
// enum types
ZYAN_STATIC_ASSERT(ZYDIS_VEX_STATIC_BROADCAST_REQUIRED_BITS  <=  8);

/**
 * Defines the `ZydisInstructionDefinitionVEX` struct.
 */
typedef struct ZydisInstructionDefinitionVEX_
{
    ZYDIS_INSTRUCTION_DEFINITION_BASE_VECTOR_INTEL;
#ifndef ZYDIS_MINIMAL_MODE
    ZyanU8 broadcast                       ZYAN_BITFIELD(ZYDIS_VEX_STATIC_BROADCAST_REQUIRED_BITS);
#endif
} ZydisInstructionDefinitionVEX;

#ifndef ZYDIS_DISABLE_AVX512

// MSVC does not correctly execute the `pragma pack(1)` compiler-directive, if we use the correct
// enum types
ZYAN_STATIC_ASSERT(ZYDIS_IVECTOR_LENGTH_REQUIRED_BITS        <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_TUPLETYPE_REQUIRED_BITS             <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_IELEMENT_SIZE_REQUIRED_BITS         <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_EVEX_FUNC_REQUIRED_BITS             <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_MASK_POLICY_REQUIRED_BITS           <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_MASK_OVERRIDE_REQUIRED_BITS         <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_EVEX_STATIC_BROADCAST_REQUIRED_BITS <=  8);

/**
 * Defines the `ZydisInstructionDefinitionEVEX` struct.
 */
typedef struct ZydisInstructionDefinitionEVEX_
{
    ZYDIS_INSTRUCTION_DEFINITION_BASE_VECTOR_INTEL;
#ifndef ZYDIS_MINIMAL_MODE
    ZyanU8 vector_length                   ZYAN_BITFIELD(ZYDIS_IVECTOR_LENGTH_REQUIRED_BITS);
    ZyanU8 tuple_type                      ZYAN_BITFIELD(ZYDIS_TUPLETYPE_REQUIRED_BITS);
    ZyanU8 element_size                    ZYAN_BITFIELD(ZYDIS_IELEMENT_SIZE_REQUIRED_BITS);
    ZyanU8 functionality                   ZYAN_BITFIELD(ZYDIS_EVEX_FUNC_REQUIRED_BITS);
#endif
    ZyanU8 mask_policy                     ZYAN_BITFIELD(ZYDIS_MASK_POLICY_REQUIRED_BITS);
    ZyanBool accepts_zero_mask             ZYAN_BITFIELD( 1);
#ifndef ZYDIS_MINIMAL_MODE
    ZyanU8 mask_override                   ZYAN_BITFIELD(ZYDIS_MASK_OVERRIDE_REQUIRED_BITS);
    ZyanU8 broadcast                       ZYAN_BITFIELD(ZYDIS_EVEX_STATIC_BROADCAST_REQUIRED_BITS);
#endif
} ZydisInstructionDefinitionEVEX;
#endif

#ifndef ZYDIS_DISABLE_KNC

// MSVC does not correctly execute the `pragma pack(1)` compiler-directive, if we use the correct
// enum types
ZYAN_STATIC_ASSERT(ZYDIS_MVEX_FUNC_REQUIRED_BITS             <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_MASK_POLICY_REQUIRED_BITS           <=  8);
ZYAN_STATIC_ASSERT(ZYDIS_MVEX_STATIC_BROADCAST_REQUIRED_BITS <=  8);

/**
 * Defines the `ZydisInstructionDefinitionMVEX` struct.
 */
typedef struct ZydisInstructionDefinitionMVEX_
{
    ZYDIS_INSTRUCTION_DEFINITION_BASE_VECTOR_INTEL;
    ZyanU8 functionality                   ZYAN_BITFIELD(ZYDIS_MVEX_FUNC_REQUIRED_BITS);
    ZyanU8 mask_policy                     ZYAN_BITFIELD(ZYDIS_MASK_POLICY_REQUIRED_BITS);
#ifndef ZYDIS_MINIMAL_MODE
    ZyanBool has_element_granularity       ZYAN_BITFIELD( 1);
    ZyanU8 broadcast                       ZYAN_BITFIELD(ZYDIS_MVEX_STATIC_BROADCAST_REQUIRED_BITS);
#endif
} ZydisInstructionDefinitionMVEX;
#endif

/* ---------------------------------------------------------------------------------------------- */

#pragma pack(pop)

#ifdef ZYAN_MSVC
#   pragma warning(pop)
#endif

/* ---------------------------------------------------------------------------------------------- */
/* Accessed CPU/FPU flags                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/*
 * Contains information about the CPU/FPU flags accessed by an instruction.
 *
 * We don't want this struct to be packed! A pointer to the individual members will be used by the
 * `ZydisDecodedInstruction` struct.
 */
typedef struct ZydisDefinitionAccessedFlags_
{
    ZydisAccessedFlags cpu_flags;
    ZydisAccessedFlags fpu_flags;
} ZydisDefinitionAccessedFlags;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Functions                                                                                      */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Instruction definition                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the instruction-definition with the given `encoding` and `id`.
 *
 * @param   encoding    The instruction-encoding.
 * @param   id          The definition-id.
 * @param   definition  A pointer to the variable that receives a pointer to the instruction-
 *                      definition.
 */
ZYDIS_NO_EXPORT void ZydisGetInstructionDefinition(ZydisInstructionEncoding encoding,
    ZyanU16 id, const ZydisInstructionDefinition** definition);

/* ---------------------------------------------------------------------------------------------- */
/* Operand definition                                                                             */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Returns the the operand-definitions for the given instruction-`definition`.
 *
 * @param   definition  A pointer to the instruction-definition.
 *
 * @return  A pointer to the first operand definition of the instruction, or `ZYAN_NULL`.
 */
ZYDIS_NO_EXPORT const ZydisOperandDefinition* ZydisGetOperandDefinitions(
    const ZydisInstructionDefinition* definition);
#endif

/* ---------------------------------------------------------------------------------------------- */
/* Element info                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Returns the actual type and size of an internal element-type.
 *
 * @param   element The internal element type.
 * @param   type    The actual element type.
 * @param   size    The element size.
 */
ZYDIS_NO_EXPORT void ZydisGetElementInfo(ZydisInternalElementType element, ZydisElementType* type,
    ZydisElementSize* size);
#endif

/* ---------------------------------------------------------------------------------------------- */
/* Accessed CPU flags                                                                             */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Returns the the operand-definitions for the given instruction-`definition`.
 *
 * @param   definition  A pointer to the instruction-definition.
 * @param   flags       A pointer to the variable that receives the `ZydisDefinitionAccessedFlags`
 *                      struct.
 *
 * @return  `ZYAN_TRUE`, if the instruction accesses any flags, or `ZYAN_FALSE`, if not.
 */
ZYDIS_NO_EXPORT ZyanBool ZydisGetAccessedFlags(const ZydisInstructionDefinition* definition,
    const ZydisDefinitionAccessedFlags** flags);
#endif

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_INTERNAL_SHAREDDATA_H */
