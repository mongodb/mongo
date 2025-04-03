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
 * Utility functions and constants for registers.
 */

#ifndef ZYDIS_REGISTER_H
#define ZYDIS_REGISTER_H

#include "zydis/Zycore/Defines.h"
#include "zydis/Zycore/Types.h"
#include "zydis/Zydis/Defines.h"
#include "zydis/Zydis/SharedTypes.h"
#include "zydis/Zydis/ShortString.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Registers                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

#include "zydis/Zydis/Generated/EnumRegister.h"

/* ---------------------------------------------------------------------------------------------- */
/* Register kinds                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisRegisterKind` enum.
 *
 * Please note that this enum does not contain a matching entry for all values of the
 * `ZydisRegister` enum, but only for those registers where it makes sense to logically group them
 * for decoding/encoding purposes.
 *
 * These are mainly the registers that can be identified by an id within their corresponding
 * register-class.
 */
typedef enum ZydisRegisterKind_
{
    ZYDIS_REGKIND_INVALID,
    ZYDIS_REGKIND_GPR,
    ZYDIS_REGKIND_X87,
    ZYDIS_REGKIND_MMX,
    ZYDIS_REGKIND_VR,
    ZYDIS_REGKIND_TMM,
    ZYDIS_REGKIND_SEGMENT,
    ZYDIS_REGKIND_TEST,
    ZYDIS_REGKIND_CONTROL,
    ZYDIS_REGKIND_DEBUG,
    ZYDIS_REGKIND_MASK,
    ZYDIS_REGKIND_BOUND,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_REGKIND_MAX_VALUE = ZYDIS_REGKIND_BOUND,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_REGKIND_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_REGKIND_MAX_VALUE)
} ZydisRegisterKind;

/* ---------------------------------------------------------------------------------------------- */
/* Register classes                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisRegisterClass` enum.
 *
 * Please note that this enum does not contain a matching entry for all values of the
 * `ZydisRegister` enum, but only for those registers where it makes sense to logically group them
 * for decoding/encoding purposes.
 *
 * These are mainly the registers that can be identified by an id within their corresponding
 * register-class. The `IP` and `FLAGS` values are exceptions to this rule.
 */
typedef enum ZydisRegisterClass_
{
    ZYDIS_REGCLASS_INVALID,
    /**
     * 8-bit general-purpose registers.
     */
    ZYDIS_REGCLASS_GPR8,
    /**
     * 16-bit general-purpose registers.
     */
    ZYDIS_REGCLASS_GPR16,
    /**
     * 32-bit general-purpose registers.
     */
    ZYDIS_REGCLASS_GPR32,
    /**
     * 64-bit general-purpose registers.
     */
    ZYDIS_REGCLASS_GPR64,
    /**
     * Floating point legacy registers.
     */
    ZYDIS_REGCLASS_X87,
    /**
     * Floating point multimedia registers.
     */
    ZYDIS_REGCLASS_MMX,
    /**
     * 128-bit vector registers.
     */
    ZYDIS_REGCLASS_XMM,
    /**
     * 256-bit vector registers.
     */
    ZYDIS_REGCLASS_YMM,
    /**
     * 512-bit vector registers.
     */
    ZYDIS_REGCLASS_ZMM,
    /**
     * Matrix registers.
     */
    ZYDIS_REGCLASS_TMM,
    /*
     * Flags registers.
     */
    ZYDIS_REGCLASS_FLAGS,
    /**
     * Instruction-pointer registers.
     */
    ZYDIS_REGCLASS_IP,
    /**
     * Segment registers.
     */
    ZYDIS_REGCLASS_SEGMENT,
    /**
     * Table registers.
    */
    ZYDIS_REGCLASS_TABLE,
    /**
     * Test registers.
     */
    ZYDIS_REGCLASS_TEST,
    /**
     * Control registers.
     */
    ZYDIS_REGCLASS_CONTROL,
    /**
     * Debug registers.
     */
    ZYDIS_REGCLASS_DEBUG,
    /**
     * Mask registers.
     */
    ZYDIS_REGCLASS_MASK,
    /**
     * Bound registers.
     */
    ZYDIS_REGCLASS_BOUND,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_REGCLASS_MAX_VALUE = ZYDIS_REGCLASS_BOUND,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_REGCLASS_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_REGCLASS_MAX_VALUE)
} ZydisRegisterClass;

/* ---------------------------------------------------------------------------------------------- */
/* Register width                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisRegisterWidth` data-type.
 */
typedef ZyanU16 ZydisRegisterWidth;

/* ---------------------------------------------------------------------------------------------- */
/* Register context                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisRegisterContext` struct.
 */
typedef struct ZydisRegisterContext_
{
    /**
     * The values stored in the register context.
     */
    ZyanU64 values[ZYDIS_REGISTER_MAX_VALUE + 1];
} ZydisRegisterContext;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * @addtogroup register Register
 * Functions allowing retrieval of information about registers.
 * @{
 */

/* ---------------------------------------------------------------------------------------------- */
/* Register                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the register specified by the `register_class` and `id` tuple.
 *
 * @param   register_class  The register class.
 * @param   id              The register id.
 *
 * @return  The register specified by the `register_class` and `id` tuple or `ZYDIS_REGISTER_NONE`,
 *          if an invalid parameter was passed.
 */
ZYDIS_EXPORT ZydisRegister ZydisRegisterEncode(ZydisRegisterClass register_class, ZyanU8 id);

/**
 * Returns the id of the specified register.
 *
 * @param   reg The register.
 *
 * @return  The id of the specified register, or -1 if an invalid parameter was passed.
 */
ZYDIS_EXPORT ZyanI8 ZydisRegisterGetId(ZydisRegister reg);

/**
 * Returns the register-class of the specified register.
 *
 * @param   reg The register.
 *
 * @return  The register-class of the specified register.
 */
ZYDIS_EXPORT ZydisRegisterClass ZydisRegisterGetClass(ZydisRegister reg);

/**
 * Returns the width of the specified register.
 *
 * @param   mode    The active machine mode.
 * @param   reg     The register.
 *
 * @return  The width of the specified register, or `ZYDIS_REGISTER_NONE` if the register is
 *          invalid for the active machine-mode.
 */
ZYDIS_EXPORT ZydisRegisterWidth ZydisRegisterGetWidth(ZydisMachineMode mode, ZydisRegister reg);

/**
 * Returns the largest enclosing register of the given register.
 *
 * @param   mode    The active machine mode.
 * @param   reg     The register.
 *
 * @return  The largest enclosing register of the given register, or `ZYDIS_REGISTER_NONE` if the
 *          register is invalid for the active machine-mode or does not have an enclosing-register.
 */
ZYDIS_EXPORT ZydisRegister ZydisRegisterGetLargestEnclosing(ZydisMachineMode mode,
    ZydisRegister reg);

/**
 * Returns the specified register string.
 *
 * @param   reg The register.
 *
 * @return  The register string or `ZYAN_NULL`, if an invalid register was passed.
 */
ZYDIS_EXPORT const char* ZydisRegisterGetString(ZydisRegister reg);

/**
 * Returns the specified register string as `ZydisShortString`.
 *
 * @param   reg The register.
 *
 * @return  The register string or `ZYAN_NULL`, if an invalid register was passed.
 *
 * The `buffer` of the returned struct is guaranteed to be zero-terminated in this special case.
 */
ZYDIS_EXPORT const ZydisShortString* ZydisRegisterGetStringWrapped(ZydisRegister reg);

/* ---------------------------------------------------------------------------------------------- */
/* Register class                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the width of the specified register-class.
 *
 * @param   mode            The active machine mode.
 * @param   register_class  The register class.
 *
 * @return  The width of the specified register.
 */
ZYDIS_EXPORT ZydisRegisterWidth ZydisRegisterClassGetWidth(ZydisMachineMode mode,
    ZydisRegisterClass register_class);

/* ---------------------------------------------------------------------------------------------- */

/**
 * @}
 */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_REGISTER_H */
