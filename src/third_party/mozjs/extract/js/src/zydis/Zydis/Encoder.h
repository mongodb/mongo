/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Mappa

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
 * Functions for encoding instructions.
 */

#ifndef ZYDIS_ENCODER_H
#define ZYDIS_ENCODER_H

#include "zydis/Zycore/Types.h"
#include "zydis/Zydis/MetaInfo.h"
#include "zydis/Zydis/Register.h"
#include "zydis/Zydis/DecoderTypes.h"
#include "zydis/Zydis/Mnemonic.h"
#include "zydis/Zydis/Status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constants                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Maximum number of encodable (explicit and implicit) operands
 */
#define ZYDIS_ENCODER_MAX_OPERANDS 5

// If asserts are failing here remember to update encoder table generator before fixing asserts
ZYAN_STATIC_ASSERT(ZYAN_BITS_TO_REPRESENT(ZYDIS_ENCODER_MAX_OPERANDS) == 3);

/**
 * Combination of all user-encodable prefixes
 */
#define ZYDIS_ENCODABLE_PREFIXES   (ZYDIS_ATTRIB_HAS_LOCK | \
                                    ZYDIS_ATTRIB_HAS_REP | \
                                    ZYDIS_ATTRIB_HAS_REPE | \
                                    ZYDIS_ATTRIB_HAS_REPNE | \
                                    ZYDIS_ATTRIB_HAS_BND | \
                                    ZYDIS_ATTRIB_HAS_XACQUIRE | \
                                    ZYDIS_ATTRIB_HAS_XRELEASE | \
                                    ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN | \
                                    ZYDIS_ATTRIB_HAS_BRANCH_TAKEN | \
                                    ZYDIS_ATTRIB_HAS_NOTRACK | \
                                    ZYDIS_ATTRIB_HAS_SEGMENT_CS | \
                                    ZYDIS_ATTRIB_HAS_SEGMENT_SS | \
                                    ZYDIS_ATTRIB_HAS_SEGMENT_DS | \
                                    ZYDIS_ATTRIB_HAS_SEGMENT_ES | \
                                    ZYDIS_ATTRIB_HAS_SEGMENT_FS | \
                                    ZYDIS_ATTRIB_HAS_SEGMENT_GS)

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines possible physical instruction encodings as bit flags, so multiple acceptable encodings
 * can be specified simultaneously.
 */
typedef enum ZydisEncodableEncoding_
{
    ZYDIS_ENCODABLE_ENCODING_DEFAULT                = 0x00000000,
    ZYDIS_ENCODABLE_ENCODING_LEGACY                 = 0x00000001,
    ZYDIS_ENCODABLE_ENCODING_3DNOW                  = 0x00000002,
    ZYDIS_ENCODABLE_ENCODING_XOP                    = 0x00000004,
    ZYDIS_ENCODABLE_ENCODING_VEX                    = 0x00000008,
    ZYDIS_ENCODABLE_ENCODING_EVEX                   = 0x00000010,
    ZYDIS_ENCODABLE_ENCODING_MVEX                   = 0x00000020,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_ENCODABLE_ENCODING_MAX_VALUE              = (ZYDIS_ENCODABLE_ENCODING_MVEX | 
                                                       (ZYDIS_ENCODABLE_ENCODING_MVEX - 1)),
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_ENCODABLE_ENCODING_REQUIRED_BITS          = 
        ZYAN_BITS_TO_REPRESENT(ZYDIS_ENCODABLE_ENCODING_MAX_VALUE)
} ZydisEncodableEncoding;

/**
 * Defines encodable physical/effective sizes of relative immediate operands. See
 * `ZydisEncoderRequest.branch_width` for more details.
 */
typedef enum ZydisBranchWidth_
{
    ZYDIS_BRANCH_WIDTH_NONE,
    ZYDIS_BRANCH_WIDTH_8,
    ZYDIS_BRANCH_WIDTH_16,
    ZYDIS_BRANCH_WIDTH_32,
    ZYDIS_BRANCH_WIDTH_64,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_BRANCH_WIDTH_MAX_VALUE = ZYDIS_BRANCH_WIDTH_64,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_BRANCH_WIDTH_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_BRANCH_WIDTH_MAX_VALUE)
} ZydisBranchWidth;

/**
 * Defines possible values for address size hints. See `ZydisEncoderRequest` for more information
 * about address size hints.
 */
typedef enum ZydisAddressSizeHint_
{
    ZYDIS_ADDRESS_SIZE_HINT_NONE,
    ZYDIS_ADDRESS_SIZE_HINT_16,
    ZYDIS_ADDRESS_SIZE_HINT_32,
    ZYDIS_ADDRESS_SIZE_HINT_64,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_ADDRESS_SIZE_HINT_MAX_VALUE = ZYDIS_ADDRESS_SIZE_HINT_64,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_ADDRESS_SIZE_HINT_REQUIRED_BITS =
        ZYAN_BITS_TO_REPRESENT(ZYDIS_ADDRESS_SIZE_HINT_MAX_VALUE)
} ZydisAddressSizeHint;

/**
 * Defines possible values for operand size hints. See `ZydisEncoderRequest` for more information
 * about operand size hints.
 */
typedef enum ZydisOperandSizeHint_
{
    ZYDIS_OPERAND_SIZE_HINT_NONE,
    ZYDIS_OPERAND_SIZE_HINT_8,
    ZYDIS_OPERAND_SIZE_HINT_16,
    ZYDIS_OPERAND_SIZE_HINT_32,
    ZYDIS_OPERAND_SIZE_HINT_64,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_OPERAND_SIZE_HINT_MAX_VALUE = ZYDIS_OPERAND_SIZE_HINT_64,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_OPERAND_SIZE_HINT_REQUIRED_BITS =
        ZYAN_BITS_TO_REPRESENT(ZYDIS_OPERAND_SIZE_HINT_MAX_VALUE)
} ZydisOperandSizeHint;

/**
 * Describes explicit or implicit instruction operand.
 */
typedef struct ZydisEncoderOperand_
{
    /**
     * The type of the operand.
     */
    ZydisOperandType type;
    /**
     * Extended info for register-operands.
     */
    struct ZydisEncoderOperandReg_
    {
        /**
         * The register value.
         */
        ZydisRegister value;
        /**
         * Is this 4th operand (`VEX`/`XOP`). Despite its name, `is4` encoding can sometimes be
         * applied to 3rd operand instead of 4th. This field is used to resolve such ambiguities.
         * For all other operands it should be set to `ZYAN_FALSE`.
         */
        ZyanBool is4;
    } reg;
    /**
     * Extended info for memory-operands.
     */
    struct ZydisEncoderOperandMem_
    {
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
         * The displacement value. This value is always treated as 64-bit signed integer, so it's
         * important to take this into account when specifying absolute addresses. For example
         * to specify a 16-bit address 0x8000 in 16-bit mode it should be sign extended to
         * `0xFFFFFFFFFFFF8000`. See `address_size_hint` for more information about absolute
         * addresses.
         */
        ZyanI64 displacement;
        /**
         * Size of this operand in bytes.
         */
        ZyanU16 size;
    } mem;
    /**
     * Extended info for pointer-operands.
     */
    struct ZydisEncoderOperandPtr_
    {
        /**
         * The segment value.
         */
        ZyanU16 segment;
        /**
         * The offset value.
         */
        ZyanU32 offset;
    } ptr;
    /**
     * Extended info for immediate-operands.
     */
    union ZydisEncoderOperandImm_
    {
        /**
         * The unsigned immediate value.
         */
        ZyanU64 u;
        /**
         * The signed immediate value.
         */
        ZyanI64 s;
    } imm;
} ZydisEncoderOperand;

/**
 * Main structure consumed by the encoder. It represents full semantics of an instruction.
 */
typedef struct ZydisEncoderRequest_
{
    /**
     * The machine mode used to encode this instruction.
     */
    ZydisMachineMode machine_mode;
    /**
     * This optional field can be used to restrict allowed physical encodings for desired
     * instruction. Some mnemonics can be supported by more than one encoding, so this field can
     * resolve ambiguities e.g. you can disable `AVX-512` extensions by prohibiting usage of `EVEX`
     * prefix and allow only `VEX` variants.
     */
    ZydisEncodableEncoding allowed_encodings;
    /**
     * The instruction-mnemonic.
     */
    ZydisMnemonic mnemonic;
    /**
     * A combination of requested encodable prefixes (`ZYDIS_ATTRIB_HAS_*` flags) for desired
     * instruction. See `ZYDIS_ENCODABLE_PREFIXES` for list of available prefixes.
     */
    ZydisInstructionAttributes prefixes;
    /**
     * Branch type (required for branching instructions only). Use `ZYDIS_BRANCH_TYPE_NONE` to let
     * encoder pick size-optimal branch type automatically (`short` and `near` are prioritized over
     * `far`).
     */
    ZydisBranchType branch_type;
    /**
     * Specifies physical size for relative immediate operands. Use `ZYDIS_BRANCH_WIDTH_NONE` to
     * let encoder pick size-optimal branch width automatically. For segment:offset `far` branches
     * this field applies to physical size of the offset part. For branching instructions without
     * relative operands this field affects effective operand size attribute.
     */
    ZydisBranchWidth branch_width;
    /**
     * Optional address size hint used to resolve ambiguities for some instructions. Generally
     * encoder deduces address size from `ZydisEncoderOperand` structures that represent
     * explicit and implicit operands. This hint resolves conflicts when instruction's hidden
     * operands scale with address size attribute.
     *
     * This hint is also used for instructions with absolute memory addresses (memory operands with
     * displacement and no registers). Since displacement field is a 64-bit signed integer it's not
     * possible to determine actual size of the address value in all situations. This hint
     * specifies size of the address value provided inside encoder request rather than desired
     * address size attribute of encoded instruction. Use `ZYDIS_ADDRESS_SIZE_HINT_NONE` to assume
     * address size default for specified machine mode.
     */
    ZydisAddressSizeHint address_size_hint;
    /**
     * Optional operand size hint used to resolve ambiguities for some instructions. Generally
     * encoder deduces operand size from `ZydisEncoderOperand` structures that represent
     * explicit and implicit operands. This hint resolves conflicts when instruction's hidden
     * operands scale with operand size attribute.
     */
    ZydisOperandSizeHint operand_size_hint;
    /**
     * The number of instruction-operands.
     */
    ZyanU8 operand_count;
    /**
     * Detailed info for all explicit and implicit instruction operands.
     */
    ZydisEncoderOperand operands[ZYDIS_ENCODER_MAX_OPERANDS];
    /**
     * Extended info for `EVEX` instructions.
     */
    struct ZydisEncoderRequestEvexFeatures_
    {
        /**
         * The broadcast-mode. Specify `ZYDIS_BROADCAST_MODE_INVALID` for instructions with
         * static broadcast functionality.
         */
        ZydisBroadcastMode broadcast;
        /**
         * The rounding-mode.
         */
        ZydisRoundingMode rounding;
        /**
         * Signals, if the `SAE` (suppress-all-exceptions) functionality should be enabled for 
         * the instruction.
         */
        ZyanBool sae;
        /**
         * Signals, if the zeroing-mask functionality should be enabled for the instruction.
         * Specify `ZYAN_TRUE` for instructions with forced zeroing mask.
         */
        ZyanBool zeroing_mask;
    } evex;
    /**
     * Extended info for `MVEX` instructions.
     */
    struct ZydisEncoderRequestMvexFeatures_
    {
        /**
         * The broadcast-mode.
         */
        ZydisBroadcastMode broadcast;
        /**
         * The data-conversion mode.
         */
        ZydisConversionMode conversion;
        /**
         * The rounding-mode.
         */
        ZydisRoundingMode rounding;
        /**
         * The `AVX` register-swizzle mode.
         */
        ZydisSwizzleMode swizzle;
        /**
         * Signals, if the `SAE` (suppress-all-exceptions) functionality is enabled for
         * the instruction.
         */
        ZyanBool sae;
        /**
         * Signals, if the instruction has a memory-eviction-hint (`KNC` only).
         */
        ZyanBool eviction_hint;
    } mvex;
} ZydisEncoderRequest;

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * @addtogroup encoder Encoder
 * Functions allowing encoding of instruction bytes from a machine interpretable struct.
 * @{
 */

/**
 * Encodes instruction with semantics specified in encoder request structure.
 *
 * @param   request     A pointer to the `ZydisEncoderRequest` struct.
 * @param   buffer      A pointer to the output buffer receiving encoded instruction.
 * @param   length      A pointer to the variable containing length of the output buffer. Upon 
 *                      successful return this variable receives length of the encoded instruction.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisEncoderEncodeInstruction(const ZydisEncoderRequest *request, 
    void *buffer, ZyanUSize *length);

/**
 * Encodes instruction with semantics specified in encoder request structure. This function expects
 * absolute addresses inside encoder request instead of `EIP`/`RIP`-relative values. Function
 * predicts final instruction length prior to encoding and writes back calculated relative operands
 * to provided encoder request.
 *
 * @param   request         A pointer to the `ZydisEncoderRequest` struct.
 * @param   buffer          A pointer to the output buffer receiving encoded instruction.
 * @param   length          A pointer to the variable containing length of the output buffer. Upon
 *                          successful return this variable receives length of the encoded
 *                          instruction.
 * @param   runtime_address The runtime address of the instruction.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisEncoderEncodeInstructionAbsolute(ZydisEncoderRequest *request,
    void *buffer, ZyanUSize *length, ZyanU64 runtime_address);

/**
 * Converts decoded instruction to encoder request that can be passed to
 * `ZydisEncoderEncodeInstruction`.
 *
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operands        A pointer to the decoded operands.
 * @param   operand_count   The operand count.
 * @param   request         A pointer to the `ZydisEncoderRequest` struct, that receives
 *                          information necessary for encoder to re-encode the instruction.
 *
 * This function performs simple structure conversion and does minimal sanity checks on the 
 * input. There's no guarantee that produced request will be accepted by
 * `ZydisEncoderEncodeInstruction` if malformed `ZydisDecodedInstruction` or malformed
 * `ZydisDecodedOperands` is passed to this function.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisEncoderDecodedInstructionToEncoderRequest(
    const ZydisDecodedInstruction* instruction, const ZydisDecodedOperand* operands,
    ZyanU8 operand_count, ZydisEncoderRequest* request);

/**
 * Fills provided buffer with `NOP` instructions using longest possible multi-byte instructions.
 *
 * @param   buffer  A pointer to the output buffer receiving encoded instructions.
 * @param   length  Size of the output buffer.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisEncoderNopFill(void *buffer, ZyanUSize length);

/** @} */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_ENCODER_H */
