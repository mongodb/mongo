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

#include "zydis/Zydis/Register.h"

/* ============================================================================================== */
/* Register strings                                                                               */
/* ============================================================================================== */

#include "zydis/Zydis/Generated/EnumRegister.inc"

/* ============================================================================================== */
/* Register-class mapping                                                                         */
/* ============================================================================================== */

/**
 * Defines the `ZydisRegisterMapItem` struct.
 */
typedef struct ZydisRegisterLookupItem
{
    /**
     * The register class.
     */
    ZydisRegisterClass class;
    /**
     * The register id.
     */
    ZyanI8 id;
    /**
     * The width of register 16- and 32-bit mode.
     */
    ZydisRegisterWidth width;
    /**
     * The width of register in 64-bit mode.
     */
    ZydisRegisterWidth width64;
} ZydisRegisterLookupItem;

#include "zydis/Zydis/Generated/RegisterLookup.inc"

/**
 * Defines the `ZydisRegisterClassLookupItem` struct.
 */
typedef struct ZydisRegisterClassLookupItem_
{
    /**
     * The lowest register of the current class.
     */
    ZydisRegister lo;
    /**
     * The highest register of the current class.
     */
    ZydisRegister hi;
    /**
     * The width of registers of the current class in 16- and 32-bit mode.
     */
    ZydisRegisterWidth width;
    /**
     * The width of registers of the current class in 64-bit mode.
     */
    ZydisRegisterWidth width64;
} ZydisRegisterClassLookupItem;

#include "zydis/Zydis/Generated/RegisterClassLookup.inc"

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Register                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

ZydisRegister ZydisRegisterEncode(ZydisRegisterClass register_class, ZyanU8 id)
{
    if ((register_class == ZYDIS_REGCLASS_INVALID) ||
        (register_class == ZYDIS_REGCLASS_FLAGS) ||
        (register_class == ZYDIS_REGCLASS_IP))
    {
        return ZYDIS_REGISTER_NONE;
    }

    if ((ZyanUSize)register_class >= ZYAN_ARRAY_LENGTH(REG_CLASS_LOOKUP))
    {
        return ZYDIS_REGISTER_NONE;
    }

    const ZydisRegisterClassLookupItem* item = &REG_CLASS_LOOKUP[register_class];
    if (id <= (item->hi - item->lo))
    {
        return item->lo + id;
    }

    return ZYDIS_REGISTER_NONE;
}

ZyanI8 ZydisRegisterGetId(ZydisRegister reg)
{
    if ((ZyanUSize)reg >= ZYAN_ARRAY_LENGTH(REG_LOOKUP))
    {
        return -1;
    }

    return REG_LOOKUP[reg].id;
}

ZydisRegisterClass ZydisRegisterGetClass(ZydisRegister reg)
{
    if ((ZyanUSize)reg >= ZYAN_ARRAY_LENGTH(REG_LOOKUP))
    {
        return ZYDIS_REGCLASS_INVALID;
    }

    return REG_LOOKUP[reg].class;
}

ZydisRegisterWidth ZydisRegisterGetWidth(ZydisMachineMode mode, ZydisRegister reg)
{
    if ((ZyanUSize)reg >= ZYAN_ARRAY_LENGTH(REG_LOOKUP))
    {
        return 0;
    }

    return (mode == ZYDIS_MACHINE_MODE_LONG_64)
        ? REG_LOOKUP[reg].width64
        : REG_LOOKUP[reg].width;
}

ZydisRegister ZydisRegisterGetLargestEnclosing(ZydisMachineMode mode, ZydisRegister reg)
{
    if ((ZyanUSize)reg >= ZYAN_ARRAY_LENGTH(REG_LOOKUP))
    {
        return ZYDIS_REGISTER_NONE;
    }

    if (mode > ZYDIS_MACHINE_MODE_MAX_VALUE)
    {
        return ZYDIS_REGISTER_NONE;
    }

    const ZydisRegisterClass reg_class = REG_LOOKUP[reg].class;

    if ((reg_class == ZYDIS_REGCLASS_INVALID) ||
        ((reg_class == ZYDIS_REGCLASS_GPR64) && (mode != ZYDIS_MACHINE_MODE_LONG_64)))
    {
        return ZYDIS_REGISTER_NONE;
    }

    static const ZydisRegister STATIC_MAPPING[ZYDIS_REGCLASS_MAX_VALUE + 1][3] =
    {
                                 /* 16              */ /* 32               */ /* 64                  */
        [ZYDIS_REGCLASS_FLAGS] = { ZYDIS_REGISTER_FLAGS, ZYDIS_REGISTER_EFLAGS, ZYDIS_REGISTER_RFLAGS },
        [ZYDIS_REGCLASS_IP   ] = { ZYDIS_REGISTER_IP   , ZYDIS_REGISTER_EIP   , ZYDIS_REGISTER_RIP    },
    };
    ZYAN_ASSERT(reg_class < ZYAN_ARRAY_LENGTH(STATIC_MAPPING));

    ZyanU8 mode_bits;
    switch (mode)
    {
    case ZYDIS_MACHINE_MODE_LONG_64:
        mode_bits = 2;
        break;
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
    case ZYDIS_MACHINE_MODE_LEGACY_32:
        mode_bits = 1;
        break;
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
    case ZYDIS_MACHINE_MODE_LEGACY_16:
    case ZYDIS_MACHINE_MODE_REAL_16:
        mode_bits = 0;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    const ZydisRegister static_reg = STATIC_MAPPING[reg_class][mode_bits];
    if (static_reg != ZYDIS_REGISTER_NONE)
    {
        return static_reg;
    }

    static const ZyanU8 GPR8_MAPPING[20] =
    {
        /* AL   */  0,
        /* CL   */  1,
        /* DL   */  2,
        /* BL   */  3,
        /* AH   */  0,
        /* CH   */  1,
        /* DH   */  2,
        /* BH   */  3,
        /* SPL  */  4,
        /* BPL  */  5,
        /* SIL  */  6,
        /* DIL  */  7,
        /* R8B  */  8,
        /* R9B  */  9,
        /* R10B */ 10,
        /* R11B */ 11,
        /* R12B */ 12,
        /* R13B */ 13,
        /* R14B */ 14,
        /* R15B */ 15
    };

    ZyanU8 reg_id = REG_LOOKUP[reg].id;
    switch (reg_class)
    {
    case ZYDIS_REGCLASS_GPR8:
        reg_id = GPR8_MAPPING[reg_id];
        ZYAN_FALLTHROUGH;
    case ZYDIS_REGCLASS_GPR16:
    case ZYDIS_REGCLASS_GPR32:
    case ZYDIS_REGCLASS_GPR64:
        switch (mode_bits)
        {
        case 2:
            return REG_CLASS_LOOKUP[ZYDIS_REGCLASS_GPR64].lo + reg_id;
        case 1:
            return REG_CLASS_LOOKUP[ZYDIS_REGCLASS_GPR32].lo + reg_id;
        case 0:
            return REG_CLASS_LOOKUP[ZYDIS_REGCLASS_GPR16].lo + reg_id;
        default:
            ZYAN_UNREACHABLE;
        }
    case ZYDIS_REGCLASS_XMM:
    case ZYDIS_REGCLASS_YMM:
    case ZYDIS_REGCLASS_ZMM:
#if defined(ZYDIS_DISABLE_AVX512) && defined(ZYDIS_DISABLE_KNC)
        return REG_CLASS_LOOKUP[ZYDIS_REGCLASS_YMM].lo + reg_id;
#else
        return REG_CLASS_LOOKUP[ZYDIS_REGCLASS_ZMM].lo + reg_id;
#endif
    default:
        return ZYDIS_REGISTER_NONE;
    }
}

const char* ZydisRegisterGetString(ZydisRegister reg)
{
    if ((ZyanUSize)reg >= ZYAN_ARRAY_LENGTH(STR_REGISTERS))
    {
        return ZYAN_NULL;
    }

    return STR_REGISTERS[reg].data;
}

const ZydisShortString* ZydisRegisterGetStringWrapped(ZydisRegister reg)
{
    if ((ZyanUSize)reg >= ZYAN_ARRAY_LENGTH(STR_REGISTERS))
    {
        return ZYAN_NULL;
    }

    return &STR_REGISTERS[reg];
}

/* ---------------------------------------------------------------------------------------------- */
/* Register class                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

ZydisRegisterWidth ZydisRegisterClassGetWidth(ZydisMachineMode mode,
    ZydisRegisterClass register_class)
{
    if ((ZyanUSize)register_class >= ZYAN_ARRAY_LENGTH(REG_CLASS_LOOKUP))
    {
        return 0;
    }

    return (mode == ZYDIS_MACHINE_MODE_LONG_64)
        ? REG_CLASS_LOOKUP[register_class].width64
        : REG_CLASS_LOOKUP[register_class].width;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
