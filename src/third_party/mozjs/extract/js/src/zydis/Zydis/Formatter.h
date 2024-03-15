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
 * Functions for formatting instructions to human-readable text.
 */

#ifndef ZYDIS_FORMATTER_H
#define ZYDIS_FORMATTER_H

#include "zydis/Zycore/Defines.h"
#include "zydis/Zycore/String.h"
#include "zydis/Zycore/Types.h"
#include "zydis/Zydis/DecoderTypes.h"
#include "zydis/Zydis/FormatterBuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Constants                                                                                      */
/* ============================================================================================== */

/**
 * Use this constant as value for `runtime_address` in `ZydisFormatterFormatInstruction(Ex)`
 * or `ZydisFormatterFormatOperand(Ex)` to print relative values for all addresses.
 */
#define ZYDIS_RUNTIME_ADDRESS_NONE (ZyanU64)(-1)

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Formatter style                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Enum selecting the syntax to format the disassembly in.
 */
typedef enum ZydisFormatterStyle_
{
    /**
     * Generates `AT&T`-style disassembly.
     */
    ZYDIS_FORMATTER_STYLE_ATT,
    /**
     * Generates `Intel`-style disassembly.
     */
    ZYDIS_FORMATTER_STYLE_INTEL,
    /**
     * Generates `MASM`-style disassembly that is directly accepted as input for
     * the `MASM` assembler.
     *
     * The runtime-address is ignored in this mode.
     */
    ZYDIS_FORMATTER_STYLE_INTEL_MASM,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_FORMATTER_STYLE_MAX_VALUE = ZYDIS_FORMATTER_STYLE_INTEL_MASM,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_FORMATTER_STYLE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_FORMATTER_STYLE_MAX_VALUE)
} ZydisFormatterStyle;

/* ---------------------------------------------------------------------------------------------- */
/* Properties                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Enum selecting a property of the formatter.
 */
typedef enum ZydisFormatterProperty_
{
    /* ---------------------------------------------------------------------------------------- */
    /* General                                                                                  */
    /* ---------------------------------------------------------------------------------------- */

    /**
     * Controls the printing of effective operand-size suffixes (`AT&T`) or operand-sizes
     * of memory operands (`INTEL`).
     *
     * Pass `ZYAN_TRUE` as value to force the formatter to always print the size, or `ZYAN_FALSE`
     * to only print it if needed.
     */
    ZYDIS_FORMATTER_PROP_FORCE_SIZE,
    /**
     * Controls the printing of segment prefixes.
     *
     * Pass `ZYAN_TRUE` as value to force the formatter to always print the segment register of
     * memory-operands or `ZYAN_FALSE` to omit implicit `DS`/`SS` segments.
     */
    ZYDIS_FORMATTER_PROP_FORCE_SEGMENT,
    /**
     * Controls the printing of the scale-factor component for memory operands.
     *
     * Pass `ZYAN_TRUE` as value to force the formatter to always print the scale-factor component
     * of memory operands or `ZYAN_FALSE` to omit the scale factor for values of `1`.
     */
     ZYDIS_FORMATTER_PROP_FORCE_SCALE_ONE,
    /**
     * Controls the printing of branch addresses.
     *
     * Pass `ZYAN_TRUE` as value to force the formatter to always print relative branch addresses
     * or `ZYAN_FALSE` to use absolute addresses, if a runtime-address different to
     * `ZYDIS_RUNTIME_ADDRESS_NONE` was passed.
     */
    ZYDIS_FORMATTER_PROP_FORCE_RELATIVE_BRANCHES,
    /**
     * Controls the printing of `EIP`/`RIP`-relative addresses.
     *
     * Pass `ZYAN_TRUE` as value to force the formatter to always print relative addresses for
     * `EIP`/`RIP`-relative operands or `ZYAN_FALSE` to use absolute addresses, if a runtime-
     * address different to `ZYDIS_RUNTIME_ADDRESS_NONE` was passed.
     */
    ZYDIS_FORMATTER_PROP_FORCE_RELATIVE_RIPREL,
    /**
     * Controls the printing of branch-instructions sizes.
     *
     * Pass `ZYAN_TRUE` as value to print the size (`short`, `near`) of branch
     * instructions or `ZYAN_FALSE` to hide it.
     *
     * Note that the `far`/`l` modifier is always printed.
     */
    ZYDIS_FORMATTER_PROP_PRINT_BRANCH_SIZE,

    /**
     * Controls the printing of instruction prefixes.
     *
     * Pass `ZYAN_TRUE` as value to print all instruction-prefixes (even ignored or duplicate
     * ones) or `ZYAN_FALSE` to only print prefixes that are effectively used by the instruction.
     */
    ZYDIS_FORMATTER_PROP_DETAILED_PREFIXES,

    /* ---------------------------------------------------------------------------------------- */
    /* Numeric values                                                                           */
    /* ---------------------------------------------------------------------------------------- */

    /**
     * Controls the base of address values.
     */
    ZYDIS_FORMATTER_PROP_ADDR_BASE,
    /**
     * Controls the signedness of relative addresses. Absolute addresses are
     * always unsigned.
     */
    ZYDIS_FORMATTER_PROP_ADDR_SIGNEDNESS,
    /**
     * Controls the padding of absolute address values.
     *
     * Pass `ZYDIS_PADDING_DISABLED` to disable padding, `ZYDIS_PADDING_AUTO` to padd all
     * addresses to the current stack width (hexadecimal only), or any other integer value for
     * custom padding.
     */
    ZYDIS_FORMATTER_PROP_ADDR_PADDING_ABSOLUTE,
    /**
     * Controls the padding of relative address values.
     *
     * Pass `ZYDIS_PADDING_DISABLED` to disable padding, `ZYDIS_PADDING_AUTO` to padd all
     * addresses to the current stack width (hexadecimal only), or any other integer value for
     * custom padding.
     */
    ZYDIS_FORMATTER_PROP_ADDR_PADDING_RELATIVE,

    /* ---------------------------------------------------------------------------------------- */

    /**
     * Controls the base of displacement values.
     */
    ZYDIS_FORMATTER_PROP_DISP_BASE,
    /**
     * Controls the signedness of displacement values.
     */
    ZYDIS_FORMATTER_PROP_DISP_SIGNEDNESS,
    /**
     * Controls the padding of displacement values.
     *
     * Pass `ZYDIS_PADDING_DISABLED` to disable padding, or any other integer value for custom
     * padding.
     */
    ZYDIS_FORMATTER_PROP_DISP_PADDING,

    /* ---------------------------------------------------------------------------------------- */

    /**
     * Controls the base of immediate values.
     */
    ZYDIS_FORMATTER_PROP_IMM_BASE,
    /**
     * Controls the signedness of immediate values.
     *
     * Pass `ZYDIS_SIGNEDNESS_AUTO` to automatically choose the most suitable mode based on the
     * operands `ZydisDecodedOperand.imm.is_signed` attribute.
     */
    ZYDIS_FORMATTER_PROP_IMM_SIGNEDNESS,
    /**
     * Controls the padding of immediate values.
     *
     * Pass `ZYDIS_PADDING_DISABLED` to disable padding, `ZYDIS_PADDING_AUTO` to padd all
     * immediates to the operand-width (hexadecimal only), or any other integer value for custom
     * padding.
     */
    ZYDIS_FORMATTER_PROP_IMM_PADDING,

    /* ---------------------------------------------------------------------------------------- */
    /* Text formatting                                                                          */
    /* ---------------------------------------------------------------------------------------- */

    /**
     * Controls the letter-case for prefixes.
     *
     * Pass `ZYAN_TRUE` as value to format in uppercase or `ZYAN_FALSE` to format in lowercase.
     */
    ZYDIS_FORMATTER_PROP_UPPERCASE_PREFIXES,
    /**
     * Controls the letter-case for the mnemonic.
     *
     * Pass `ZYAN_TRUE` as value to format in uppercase or `ZYAN_FALSE` to format in lowercase.
     */
    ZYDIS_FORMATTER_PROP_UPPERCASE_MNEMONIC,
    /**
     * Controls the letter-case for registers.
     *
     * Pass `ZYAN_TRUE` as value to format in uppercase or `ZYAN_FALSE` to format in lowercase.
     */
    ZYDIS_FORMATTER_PROP_UPPERCASE_REGISTERS,
    /**
     * Controls the letter-case for typecasts.
     *
     * Pass `ZYAN_TRUE` as value to format in uppercase or `ZYAN_FALSE` to format in lowercase.
     */
    ZYDIS_FORMATTER_PROP_UPPERCASE_TYPECASTS,
    /**
     * Controls the letter-case for decorators.
     *
     * Pass `ZYAN_TRUE` as value to format in uppercase or `ZYAN_FALSE` to format in lowercase.
     */
    ZYDIS_FORMATTER_PROP_UPPERCASE_DECORATORS,

    /* ---------------------------------------------------------------------------------------- */
    /* Number formatting                                                                        */
    /* ---------------------------------------------------------------------------------------- */

    /**
     * Controls the prefix for decimal values.
     *
     * Pass a pointer to a null-terminated C-style string with a maximum length of 10 characters
     * to set a custom prefix, or `ZYAN_NULL` to disable it.
     *
     * The string is deep-copied into an internal buffer.
     */
    ZYDIS_FORMATTER_PROP_DEC_PREFIX,
    /**
     * Controls the suffix for decimal values.
     *
     * Pass a pointer to a null-terminated C-style string with a maximum length of 10 characters
     * to set a custom suffix, or `ZYAN_NULL` to disable it.
     *
     * The string is deep-copied into an internal buffer.
     */
    ZYDIS_FORMATTER_PROP_DEC_SUFFIX,

    /* ---------------------------------------------------------------------------------------- */

    /**
     * Controls the letter-case of hexadecimal values.
     *
     * Pass `ZYAN_TRUE` as value to format in uppercase and `ZYAN_FALSE` to format in lowercase.
     *
     * The default value is `ZYAN_TRUE`.
     */
    ZYDIS_FORMATTER_PROP_HEX_UPPERCASE,
    /**
     * Controls whether to prepend hexadecimal values with a leading zero if the first character
     * is non-numeric.
     *
     * Pass `ZYAN_TRUE` to prepend a leading zero if the first character is non-numeric or
     * `ZYAN_FALSE` to disable this functionality.
     *
     * The default value is `ZYAN_FALSE`.
     */
    ZYDIS_FORMATTER_PROP_HEX_FORCE_LEADING_NUMBER,
    /**
     * Controls the prefix for hexadecimal values.
     *
     * Pass a pointer to a null-terminated C-style string with a maximum length of 10 characters
     * to set a custom prefix, or `ZYAN_NULL` to disable it.
     *
     * The string is deep-copied into an internal buffer.
     */
    ZYDIS_FORMATTER_PROP_HEX_PREFIX,
    /**
     * Controls the suffix for hexadecimal values.
     *
     * Pass a pointer to a null-terminated C-style string with a maximum length of 10 characters
     * to set a custom suffix, or `ZYAN_NULL` to disable it.
     *
     * The string is deep-copied into an internal buffer.
     */
    ZYDIS_FORMATTER_PROP_HEX_SUFFIX,

    /* ---------------------------------------------------------------------------------------- */

    /**
     * Maximum value of this enum.
     */
    ZYDIS_FORMATTER_PROP_MAX_VALUE = ZYDIS_FORMATTER_PROP_HEX_SUFFIX,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_FORMATTER_PROP_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_FORMATTER_PROP_MAX_VALUE)
} ZydisFormatterProperty;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Enum defining different mantissae to be used during formatting.
 */
typedef enum ZydisNumericBase_
{
    /**
     * Decimal system.
     */
    ZYDIS_NUMERIC_BASE_DEC,
    /**
     * Hexadecimal system.
     */
    ZYDIS_NUMERIC_BASE_HEX,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_NUMERIC_BASE_MAX_VALUE = ZYDIS_NUMERIC_BASE_HEX,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_NUMERIC_BASE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_NUMERIC_BASE_MAX_VALUE)
} ZydisNumericBase;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Enum defining the signeness of integers to be used during formatting.
 */
typedef enum ZydisSignedness_
{
    /**
     * Automatically choose the most suitable mode based on the operands
     * ZydisDecodedOperand.imm.is_signed` attribute.
     */
    ZYDIS_SIGNEDNESS_AUTO,
    /**
     * Force signed values.
     */
    ZYDIS_SIGNEDNESS_SIGNED,
    /**
     * Force unsigned values.
     */
    ZYDIS_SIGNEDNESS_UNSIGNED,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_SIGNEDNESS_MAX_VALUE = ZYDIS_SIGNEDNESS_UNSIGNED,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_SIGNEDNESS_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_SIGNEDNESS_MAX_VALUE)
} ZydisSignedness;

/* ---------------------------------------------------------------------------------------------- */

/**
 * Enum definining magic values that receive special treatment when used as padding properties 
 * of the formatter.
 */
typedef enum ZydisPadding_
{
    /**
     * Disables padding.
     */
    ZYDIS_PADDING_DISABLED = 0,
    /**
     * Padds the value to the current stack-width for addresses, or to the
     * operand-width for immediate values (hexadecimal only).
     */
    ZYDIS_PADDING_AUTO     = (-1),

    /**
     * Maximum value of this enum.
     */
    ZYDIS_PADDING_MAX_VALUE = ZYDIS_PADDING_AUTO,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_PADDING_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_PADDING_MAX_VALUE)
} ZydisPadding;

/* ---------------------------------------------------------------------------------------------- */
/* Function types                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Enum selecting a formatter function to be replaced with hooks.
 *
 * Do NOT change the order of the values this enum or the function fields inside the
 * `ZydisFormatter` struct.
 */
typedef enum ZydisFormatterFunction_
{
    /* ---------------------------------------------------------------------------------------- */
    /* Instruction                                                                              */
    /* ---------------------------------------------------------------------------------------- */

    /**
     * This function is invoked before the formatter formats an instruction.
     */
    ZYDIS_FORMATTER_FUNC_PRE_INSTRUCTION,
    /**
     * This function is invoked after the formatter formatted an instruction.
     */
    ZYDIS_FORMATTER_FUNC_POST_INSTRUCTION,

    /* ---------------------------------------------------------------------------------------- */

    /**
     * This function refers to the main formatting function.
     *
     * Replacing this function allows for complete custom formatting, but indirectly disables all
     * other hooks except for `ZYDIS_FORMATTER_FUNC_PRE_INSTRUCTION` and
     * `ZYDIS_FORMATTER_FUNC_POST_INSTRUCTION`.
     */
    ZYDIS_FORMATTER_FUNC_FORMAT_INSTRUCTION,

    /* ---------------------------------------------------------------------------------------- */
    /* Operands                                                                                 */
    /* ---------------------------------------------------------------------------------------- */

    /**
     * This function is invoked before the formatter formats an operand.
     */
    ZYDIS_FORMATTER_FUNC_PRE_OPERAND,
    /**
     * This function is invoked after the formatter formatted an operand.
     */
    ZYDIS_FORMATTER_FUNC_POST_OPERAND,

    /* ---------------------------------------------------------------------------------------- */

    /**
     * This function is invoked to format a register operand.
     */
    ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_REG,
    /**
     * This function is invoked to format a memory operand.
     *
     * Replacing this function might indirectly disable some specific calls to the
     * `ZYDIS_FORMATTER_FUNC_PRINT_TYPECAST`, `ZYDIS_FORMATTER_FUNC_PRINT_SEGMENT`,
     * `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS` and `ZYDIS_FORMATTER_FUNC_PRINT_DISP` functions.
     */
    ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_MEM,
    /**
     * This function is invoked to format a pointer operand.
     */
    ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_PTR,
    /**
     * This function is invoked to format an immediate operand.
     *
     * Replacing this function might indirectly disable some specific calls to the
     * `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS`, `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_REL` and
     * `ZYDIS_FORMATTER_FUNC_PRINT_IMM` functions.
     */
    ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_IMM,

    /* ---------------------------------------------------------------------------------------- */
    /* Elemental tokens                                                                         */
    /* ---------------------------------------------------------------------------------------- */

    /**
     * This function is invoked to print the instruction mnemonic.
     */
    ZYDIS_FORMATTER_FUNC_PRINT_MNEMONIC,

    /* ---------------------------------------------------------------------------------------- */

    /**
     * This function is invoked to print a register.
     */
    ZYDIS_FORMATTER_FUNC_PRINT_REGISTER,
    /**
     * This function is invoked to print absolute addresses.
     *
     * Conditionally invoked, if a runtime-address different to `ZYDIS_RUNTIME_ADDRESS_NONE` was
     * passed:
     * - `IMM` operands with relative address (e.g. `JMP`, `CALL`, ...)
     * - `MEM` operands with `EIP`/`RIP`-relative address (e.g. `MOV RAX, [RIP+0x12345678]`)
     *
     * Always invoked for:
     * - `MEM` operands with absolute address (e.g. `MOV RAX, [0x12345678]`)
     */
    ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS,
    /**
     * This function is invoked to print relative addresses.
     *
     * Conditionally invoked, if `ZYDIS_RUNTIME_ADDRESS_NONE` was passed as runtime-address:
     * - `IMM` operands with relative address (e.g. `JMP`, `CALL`, ...)
     */
    ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_REL,
    /**
     * This function is invoked to print a memory displacement value.
     *
     * If the memory displacement contains an address and a runtime-address different to
     * `ZYDIS_RUNTIME_ADDRESS_NONE` was passed, `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS` is called
     * instead.
     */
    ZYDIS_FORMATTER_FUNC_PRINT_DISP,
    /**
     * This function is invoked to print an immediate value.
     *
     * If the immediate contains an address and a runtime-address different to
     * `ZYDIS_RUNTIME_ADDRESS_NONE` was passed, `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS` is called
     * instead.
     *
     * If the immediate contains an address and `ZYDIS_RUNTIME_ADDRESS_NONE` was passed as
     * runtime-address, `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_REL` is called instead.
     */
    ZYDIS_FORMATTER_FUNC_PRINT_IMM,

    /* ---------------------------------------------------------------------------------------- */
    /* Optional tokens                                                                          */
    /* ---------------------------------------------------------------------------------------- */

    /**
     * This function is invoked to print the size of a memory operand (`INTEL` only).
     */
    ZYDIS_FORMATTER_FUNC_PRINT_TYPECAST,
    /**
     * This function is invoked to print the segment-register of a memory operand.
     */
    ZYDIS_FORMATTER_FUNC_PRINT_SEGMENT,
    /**
     * This function is invoked to print the instruction prefixes.
     */
    ZYDIS_FORMATTER_FUNC_PRINT_PREFIXES,
    /**
     * This function is invoked after formatting an operand to print a `EVEX`/`MVEX`
     * decorator.
     */
    ZYDIS_FORMATTER_FUNC_PRINT_DECORATOR,

    /* ---------------------------------------------------------------------------------------- */

    /**
     * Maximum value of this enum.
     */
    ZYDIS_FORMATTER_FUNC_MAX_VALUE = ZYDIS_FORMATTER_FUNC_PRINT_DECORATOR,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_FORMATTER_FUNC_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_FORMATTER_FUNC_MAX_VALUE)
} ZydisFormatterFunction;

/* ---------------------------------------------------------------------------------------------- */
/* Decorator types                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Enum of all decorator types.
 */
typedef enum ZydisDecorator_
{
    ZYDIS_DECORATOR_INVALID,
    /**
     * The embedded-mask decorator.
     */
    ZYDIS_DECORATOR_MASK,
    /**
     * The broadcast decorator.
     */
    ZYDIS_DECORATOR_BC,
    /**
     * The rounding-control decorator.
     */
    ZYDIS_DECORATOR_RC,
    /**
     * The suppress-all-exceptions decorator.
     */
    ZYDIS_DECORATOR_SAE,
    /**
     * The register-swizzle decorator.
     */
    ZYDIS_DECORATOR_SWIZZLE,
    /**
     * The conversion decorator.
     */
    ZYDIS_DECORATOR_CONVERSION,
    /**
     * The eviction-hint decorator.
     */
    ZYDIS_DECORATOR_EH,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_DECORATOR_MAX_VALUE = ZYDIS_DECORATOR_EH,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_DECORATOR_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_DECORATOR_MAX_VALUE)
} ZydisDecorator;

/* ---------------------------------------------------------------------------------------------- */
/* Formatter context                                                                              */
/* ---------------------------------------------------------------------------------------------- */

typedef struct ZydisFormatter_ ZydisFormatter;

/**
 * Context structure that that is passed to all formatter.
 */
typedef struct ZydisFormatterContext_
{
    /**
     * A pointer to the `ZydisDecodedInstruction` struct.
     */
    const ZydisDecodedInstruction* instruction;
    /**
     * A pointer to the first `ZydisDecodedOperand` struct of the instruction.
     */
    const ZydisDecodedOperand* operands;
    /**
     * A pointer to the `ZydisDecodedOperand` struct.
     */
    const ZydisDecodedOperand* operand;
    /**
     * The runtime address of the instruction.
     */
    ZyanU64 runtime_address;
    /**
     * A pointer to user-defined data.
     *
     * This is the value that was previously passed as the `user_data` argument to 
     * @ref ZydisFormatterFormatInstruction or @ref ZydisFormatterTokenizeOperand.
     */
    void* user_data;
} ZydisFormatterContext;

/* ---------------------------------------------------------------------------------------------- */
/* Function prototypes                                                                            */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisFormatterFunc` function prototype.
 *
 * @param   formatter   A pointer to the `ZydisFormatter` instance.
 * @param   buffer      A pointer to the `ZydisFormatterBuffer` struct.
 * @param   context     A pointer to the `ZydisFormatterContext` struct.
 *
 * @return  A zyan status code.
 *
 * Returning a status code other than `ZYAN_STATUS_SUCCESS` will immediately cause the formatting
 * process to fail (see exceptions below).
 *
 * Returning `ZYDIS_STATUS_SKIP_TOKEN` is valid for functions of the following types and will
 * instruct the formatter to omit the whole operand:
 * - `ZYDIS_FORMATTER_FUNC_PRE_OPERAND`
 * - `ZYDIS_FORMATTER_FUNC_POST_OPERAND`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_REG`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_MEM`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_PTR`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_IMM`
 *
 * This function prototype is used by functions of the following types:
 * - `ZYDIS_FORMATTER_FUNC_PRE_INSTRUCTION`
 * - `ZYDIS_FORMATTER_FUNC_POST_INSTRUCTION`
 * - `ZYDIS_FORMATTER_FUNC_PRE_OPERAND`
 * - `ZYDIS_FORMATTER_FUNC_POST_OPERAND`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_INSTRUCTION`
 * - `ZYDIS_FORMATTER_FUNC_PRINT_MNEMONIC`
 * - `ZYDIS_FORMATTER_FUNC_PRINT_PREFIXES`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_REG`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_MEM`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_PTR`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_IMM`
 * - `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS`
 * - `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_REL`
 * - `ZYDIS_FORMATTER_FUNC_PRINT_DISP`
 * - `ZYDIS_FORMATTER_FUNC_PRINT_IMM`
 * - `ZYDIS_FORMATTER_FUNC_PRINT_TYPECAST`
 * - `ZYDIS_FORMATTER_FUNC_PRINT_SEGMENT`
 */
typedef ZyanStatus (*ZydisFormatterFunc)(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context);

 /**
 * Defines the `ZydisFormatterRegisterFunc` function prototype.
 *
 * @param   formatter   A pointer to the `ZydisFormatter` instance.
 * @param   buffer      A pointer to the `ZydisFormatterBuffer` struct.
 * @param   context     A pointer to the `ZydisFormatterContext` struct.
 * @param   reg         The register.
 *
 * @return  Returning a status code other than `ZYAN_STATUS_SUCCESS` will immediately cause the
 *          formatting process to fail.
 *
 * This function prototype is used by functions of the following types:
 * - `ZYDIS_FORMATTER_FUNC_PRINT_REGISTER`.
 */
typedef ZyanStatus (*ZydisFormatterRegisterFunc)(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context, ZydisRegister reg);

/**
 * Defines the `ZydisFormatterDecoratorFunc` function prototype.
 *
 * @param   formatter   A pointer to the `ZydisFormatter` instance.
 * @param   buffer      A pointer to the `ZydisFormatterBuffer` struct.
 * @param   context     A pointer to the `ZydisFormatterContext` struct.
 * @param   decorator   The decorator type.
 *
 * @return  Returning a status code other than `ZYAN_STATUS_SUCCESS` will immediately cause the
 *          formatting process to fail.
 *
 * This function type is used for:
 * - `ZYDIS_FORMATTER_FUNC_PRINT_DECORATOR`
 */
typedef ZyanStatus (*ZydisFormatterDecoratorFunc)(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context, ZydisDecorator decorator);

/* ---------------------------------------------------------------------------------------------- */
/* Formatter struct                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Context structure keeping track of internal state of the formatter.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 *
 * Do NOT change the order of the function fields or the values of the `ZydisFormatterFunction`
 * enum.
 */
struct ZydisFormatter_
{
    /**
     * The formatter style.
     */
    ZydisFormatterStyle style;
    /**
     * The `ZYDIS_FORMATTER_PROP_FORCE_SIZE` property.
     */
    ZyanBool force_memory_size;
    /**
     * The `ZYDIS_FORMATTER_PROP_FORCE_SEGMENT` property.
     */
    ZyanBool force_memory_segment;
    /**
     * The `ZYDIS_FORMATTER_PROP_FORCE_SCALE_ONE` property.
     */
    ZyanBool force_memory_scale;
    /**
     * The `ZYDIS_FORMATTER_PROP_FORCE_RELATIVE_BRANCHES` property.
     */
    ZyanBool force_relative_branches;
    /**
     * The `ZYDIS_FORMATTER_PROP_FORCE_RELATIVE_RIPREL` property.
     */
    ZyanBool force_relative_riprel;
    /**
     * The `ZYDIS_FORMATTER_PROP_PRINT_BRANCH_SIZE` property.
     */
    ZyanBool print_branch_size;
    /**
     * The `ZYDIS_FORMATTER_DETAILED_PREFIXES` property.
     */
    ZyanBool detailed_prefixes;
    /**
     * The `ZYDIS_FORMATTER_ADDR_BASE` property.
     */
    ZydisNumericBase addr_base;
    /**
     * The `ZYDIS_FORMATTER_ADDR_SIGNEDNESS` property.
     */
    ZydisSignedness addr_signedness;
    /**
     * The `ZYDIS_FORMATTER_ADDR_PADDING_ABSOLUTE` property.
     */
    ZydisPadding addr_padding_absolute;
    /**
     * The `ZYDIS_FORMATTER_ADDR_PADDING_RELATIVE` property.
     */
    ZydisPadding addr_padding_relative;
    /**
     * The `ZYDIS_FORMATTER_DISP_BASE` property.
     */
    ZydisNumericBase disp_base;
    /**
     * The `ZYDIS_FORMATTER_DISP_SIGNEDNESS` property.
     */
    ZydisSignedness disp_signedness;
    /**
     * The `ZYDIS_FORMATTER_DISP_PADDING` property.
     */
    ZydisPadding disp_padding;
    /**
     * The `ZYDIS_FORMATTER_IMM_BASE` property.
     */
    ZydisNumericBase imm_base;
    /**
     * The `ZYDIS_FORMATTER_IMM_SIGNEDNESS` property.
     */
    ZydisSignedness imm_signedness;
    /**
     * The `ZYDIS_FORMATTER_IMM_PADDING` property.
     */
    ZydisPadding imm_padding;
    /**
     * The `ZYDIS_FORMATTER_UPPERCASE_PREFIXES` property.
     */
    ZyanI32 case_prefixes;
    /**
     * The `ZYDIS_FORMATTER_UPPERCASE_MNEMONIC` property.
     */
    ZyanI32 case_mnemonic;
    /**
     * The `ZYDIS_FORMATTER_UPPERCASE_REGISTERS` property.
     */
    ZyanI32 case_registers;
    /**
     * The `ZYDIS_FORMATTER_UPPERCASE_TYPECASTS` property.
     */
    ZyanI32 case_typecasts;
    /**
     * The `ZYDIS_FORMATTER_UPPERCASE_DECORATORS` property.
     */
    ZyanI32 case_decorators;
    /**
     * The `ZYDIS_FORMATTER_HEX_UPPERCASE` property.
     */
    ZyanBool hex_uppercase;
    /**
     * The `ZYDIS_FORMATTER_HEX_FORCE_LEADING_NUMBER` property.
     */
    ZyanBool hex_force_leading_number;
    /**
     * The number formats for all numeric bases.
     *
     * Index 0 = prefix
     * Index 1 = suffix
     */
    struct
    {
        /**
         * A pointer to the `ZyanStringView` to use as prefix/suffix.
         */
        const ZyanStringView* string;
        /**
         * The `ZyanStringView` to use as prefix/suffix
         */
        ZyanStringView string_data;
        /**
         * The actual string data.
         */
        char buffer[11];
    } number_format[ZYDIS_NUMERIC_BASE_MAX_VALUE + 1][2];
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRE_INSTRUCTION` function.
     */
    ZydisFormatterFunc func_pre_instruction;
    /**
     * The `ZYDIS_FORMATTER_FUNC_POST_INSTRUCTION` function.
     */
    ZydisFormatterFunc func_post_instruction;
    /**
     * The `ZYDIS_FORMATTER_FUNC_FORMAT_INSTRUCTION` function.
     */
    ZydisFormatterFunc func_format_instruction;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRE_OPERAND` function.
     */
    ZydisFormatterFunc func_pre_operand;
    /**
     * The `ZYDIS_FORMATTER_FUNC_POST_OPERAND` function.
     */
    ZydisFormatterFunc func_post_operand;
    /**
     * The `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_REG` function.
     */
    ZydisFormatterFunc func_format_operand_reg;
    /**
     * The `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_MEM` function.
     */
    ZydisFormatterFunc func_format_operand_mem;
    /**
     * The `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_PTR` function.
     */
    ZydisFormatterFunc func_format_operand_ptr;
    /**
     * The `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_IMM` function.
     */
    ZydisFormatterFunc func_format_operand_imm;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_MNEMONIC function.
     */
    ZydisFormatterFunc func_print_mnemonic;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_REGISTER` function.
     */
    ZydisFormatterRegisterFunc func_print_register;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS` function.
     */
    ZydisFormatterFunc func_print_address_abs;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_REL` function.
     */
    ZydisFormatterFunc func_print_address_rel;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_DISP` function.
     */
    ZydisFormatterFunc func_print_disp;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_IMM` function.
     */
    ZydisFormatterFunc func_print_imm;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_TYPECAST` function.
     */
    ZydisFormatterFunc func_print_typecast;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_SEGMENT` function.
     */
    ZydisFormatterFunc func_print_segment;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_PREFIXES` function.
     */
    ZydisFormatterFunc func_print_prefixes;
    /**
     * The `ZYDIS_FORMATTER_FUNC_PRINT_DECORATOR` function.
     */
    ZydisFormatterDecoratorFunc func_print_decorator;
};

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * @addtogroup formatter Formatter
 * Functions allowing formatting of previously decoded instructions to human readable text.
 * @{
 */

/* ---------------------------------------------------------------------------------------------- */
/* Initialization                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Initializes the given `ZydisFormatter` instance.
 *
 * @param   formatter   A pointer to the `ZydisFormatter` instance.
 * @param   style       The base formatter style (either `AT&T` or `Intel` style).
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterInit(ZydisFormatter* formatter, ZydisFormatterStyle style);

/* ---------------------------------------------------------------------------------------------- */
/* Setter                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Changes the value of the specified formatter `property`.
 *
 * @param   formatter   A pointer to the `ZydisFormatter` instance.
 * @param   property    The id of the formatter-property.
 * @param   value       The new value.
 *
 * @return  A zyan status code.
 *
 * This function returns `ZYAN_STATUS_INVALID_OPERATION` if a property can't be changed for the
 * current formatter-style.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterSetProperty(ZydisFormatter* formatter,
    ZydisFormatterProperty property, ZyanUPointer value);

/**
 * Replaces a formatter function with a custom callback and/or retrieves the currently
 * used function.
 *
 * @param   formatter   A pointer to the `ZydisFormatter` instance.
 * @param   type        The formatter function-type.
 * @param   callback    A pointer to a variable that contains the pointer of the callback function
 *                      and receives the pointer of the currently used function.
 *
 * @return  A zyan status code.
 *
 * Call this function with `callback` pointing to a `ZYAN_NULL` value to retrieve the currently
 * used function without replacing it.
 *
 * This function returns `ZYAN_STATUS_INVALID_OPERATION` if a function can't be replaced for the
 * current formatter-style.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterSetHook(ZydisFormatter* formatter,
    ZydisFormatterFunction type, const void** callback);

/* ---------------------------------------------------------------------------------------------- */
/* Formatting                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Formats the given instruction and writes it into the output buffer.
 *
 * @param   formatter       A pointer to the `ZydisFormatter` instance.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operands        A pointer to the decoded operands array.
 * @param   operand_count   The length of the `operands` array. Must be equal to or greater than
 *                          the value of `instruction->operand_count_visible`.
 * @param   buffer          A pointer to the output buffer.
 * @param   length          The length of the output buffer (in characters).
 * @param   runtime_address The runtime address of the instruction or `ZYDIS_RUNTIME_ADDRESS_NONE`
 *                          to print relative addresses.
 * @param   user_data       A pointer to user-defined data which can be used in custom formatter
 *                          callbacks. Can be `ZYAN_NULL`.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterFormatInstruction(const ZydisFormatter* formatter,
    const ZydisDecodedInstruction* instruction, const ZydisDecodedOperand* operands,
    ZyanU8 operand_count, char* buffer, ZyanUSize length, ZyanU64 runtime_address,
    void* user_data);

/**
 * Formats the given operand and writes it into the output buffer.
 *
 * @param   formatter       A pointer to the `ZydisFormatter` instance.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand         A pointer to the `ZydisDecodedOperand` struct of the operand to format.
 * @param   buffer          A pointer to the output buffer.
 * @param   length          The length of the output buffer (in characters).
 * @param   runtime_address The runtime address of the instruction or `ZYDIS_RUNTIME_ADDRESS_NONE`
 *                          to print relative addresses.
 * @param   user_data       A pointer to user-defined data which can be used in custom formatter
 *                          callbacks. Can be `ZYAN_NULL`.
 *
 * @return  A zyan status code.
 *
 * Use `ZydisFormatterFormatInstruction` or `ZydisFormatterFormatInstructionEx` to format a
 * complete instruction.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterFormatOperand(const ZydisFormatter* formatter,
    const ZydisDecodedInstruction* instruction, const ZydisDecodedOperand* operand,
    char* buffer, ZyanUSize length, ZyanU64 runtime_address, void* user_data);

/* ---------------------------------------------------------------------------------------------- */
/* Tokenizing                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Tokenizes the given instruction and writes it into the output buffer.
 *
 * @param   formatter       A pointer to the `ZydisFormatter` instance.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operands        A pointer to the decoded operands array.
 * @param   operand_count   The length of the `operands` array. Must be equal to or greater than
 *                          the value of `instruction->operand_count_visible`.
 * @param   buffer          A pointer to the output buffer.
 * @param   length          The length of the output buffer (in bytes).
 * @param   runtime_address The runtime address of the instruction or `ZYDIS_RUNTIME_ADDRESS_NONE`
 *                          to print relative addresses.
 * @param   token           Receives a pointer to the first token in the output buffer.
 * @param   user_data       A pointer to user-defined data which can be used in custom formatter
 *                          callbacks. Can be `ZYAN_NULL`.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterTokenizeInstruction(const ZydisFormatter* formatter,
    const ZydisDecodedInstruction* instruction, const ZydisDecodedOperand* operands,
    ZyanU8 operand_count, void* buffer, ZyanUSize length, ZyanU64 runtime_address,
    ZydisFormatterTokenConst** token, void* user_data);

/**
 * Tokenizes the given operand and writes it into the output buffer.
 *
 * @param   formatter       A pointer to the `ZydisFormatter` instance.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand         A pointer to the `ZydisDecodedOperand` struct of the operand to format.
 * @param   buffer          A pointer to the output buffer.
 * @param   length          The length of the output buffer (in bytes).
 * @param   runtime_address The runtime address of the instruction or `ZYDIS_RUNTIME_ADDRESS_NONE`
 *                          to print relative addresses.
 * @param   token           Receives a pointer to the first token in the output buffer.
 * @param   user_data       A pointer to user-defined data which can be used in custom formatter
 *                          callbacks. Can be `ZYAN_NULL`.
 *
 * @return  A zyan status code.
 *
 * Use `ZydisFormatterTokenizeInstruction` to tokenize a complete instruction.
 */
ZYDIS_EXPORT ZyanStatus ZydisFormatterTokenizeOperand(const ZydisFormatter* formatter,
    const ZydisDecodedInstruction* instruction, const ZydisDecodedOperand* operand,
    void* buffer, ZyanUSize length, ZyanU64 runtime_address, ZydisFormatterTokenConst** token,
    void* user_data);

/* ---------------------------------------------------------------------------------------------- */

/**
 * @}
 */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_FORMATTER_H */
