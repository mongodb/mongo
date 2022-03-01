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
 * @brief   Defines the `ZydisRegisterMapItem` struct.
 */
typedef struct ZydisRegisterMapItem_
{
    /**
     * @brief   The register class.
     */
    ZydisRegisterClass class;
    /**
     * @brief   The lowest register of the current class.
     */
    ZydisRegister lo;
    /**
     * @brief   The highest register of the current class.
     */
    ZydisRegister hi;
    /**
     * @brief   The width of registers of the current class in 16- and 32-bit mode.
     */
    ZydisRegisterWidth width;
    /**
     * @brief   The width of registers of the current class in 64-bit mode.
     */
    ZydisRegisterWidth width64;
} ZydisRegisterMapItem;

/**
 * @brief   Provides register to register-class and register-class + id to register mappings.
 */
static const ZydisRegisterMapItem REGISTER_MAP[] =
{
    { ZYDIS_REGCLASS_INVALID  , ZYDIS_REGISTER_NONE   , ZYDIS_REGISTER_NONE   ,   0   ,   0 },
    { ZYDIS_REGCLASS_GPR8     , ZYDIS_REGISTER_AL     , ZYDIS_REGISTER_R15B   ,   8   ,   8 },
    { ZYDIS_REGCLASS_GPR16    , ZYDIS_REGISTER_AX     , ZYDIS_REGISTER_R15W   ,  16   ,  16 },
    { ZYDIS_REGCLASS_GPR32    , ZYDIS_REGISTER_EAX    , ZYDIS_REGISTER_R15D   ,  32   ,  32 },
    { ZYDIS_REGCLASS_GPR64    , ZYDIS_REGISTER_RAX    , ZYDIS_REGISTER_R15    ,   0   ,  64 },
    { ZYDIS_REGCLASS_X87      , ZYDIS_REGISTER_ST0    , ZYDIS_REGISTER_ST7    ,  80   ,  80 },
    { ZYDIS_REGCLASS_MMX      , ZYDIS_REGISTER_MM0    , ZYDIS_REGISTER_MM7    ,  64   ,  64 },
    { ZYDIS_REGCLASS_XMM      , ZYDIS_REGISTER_XMM0   , ZYDIS_REGISTER_XMM31  , 128   , 128 },
    { ZYDIS_REGCLASS_YMM      , ZYDIS_REGISTER_YMM0   , ZYDIS_REGISTER_YMM31  , 256   , 256 },
    { ZYDIS_REGCLASS_ZMM      , ZYDIS_REGISTER_ZMM0   , ZYDIS_REGISTER_ZMM31  , 512   , 512 },
    { ZYDIS_REGCLASS_FLAGS    , ZYDIS_REGISTER_FLAGS  , ZYDIS_REGISTER_RFLAGS ,   0   ,   0 },
    { ZYDIS_REGCLASS_IP       , ZYDIS_REGISTER_IP     , ZYDIS_REGISTER_RIP    ,   0   ,   0 },
    { ZYDIS_REGCLASS_SEGMENT  , ZYDIS_REGISTER_ES     , ZYDIS_REGISTER_GS     ,  16   ,  16 },
    { ZYDIS_REGCLASS_TEST     , ZYDIS_REGISTER_TR0    , ZYDIS_REGISTER_TR7    ,  32   ,  32 },
    { ZYDIS_REGCLASS_CONTROL  , ZYDIS_REGISTER_CR0    , ZYDIS_REGISTER_CR15   ,  32   ,  64 },
    { ZYDIS_REGCLASS_DEBUG    , ZYDIS_REGISTER_DR0    , ZYDIS_REGISTER_DR15   ,  32   ,  64 },
    { ZYDIS_REGCLASS_MASK     , ZYDIS_REGISTER_K0     , ZYDIS_REGISTER_K7     ,   0   ,   0 },
    { ZYDIS_REGCLASS_BOUND    , ZYDIS_REGISTER_BND0   , ZYDIS_REGISTER_BND3   , 128   , 128 }
};

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Register                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

ZydisRegister ZydisRegisterEncode(ZydisRegisterClass register_class, ZyanU8 id)
{
    switch (register_class)
    {
    case ZYDIS_REGCLASS_INVALID:
    case ZYDIS_REGCLASS_FLAGS:
    case ZYDIS_REGCLASS_IP:
        break;
    default:
        if (((ZyanUSize)register_class < ZYAN_ARRAY_LENGTH(REGISTER_MAP)) &&
            (id <= (REGISTER_MAP[register_class].hi - REGISTER_MAP[register_class].lo)))
        {
            return REGISTER_MAP[register_class].lo + id;
        }
    }
    return ZYDIS_REGISTER_NONE;
}

ZyanI8 ZydisRegisterGetId(ZydisRegister reg)
{
    for (ZyanUSize i = 0; i < ZYAN_ARRAY_LENGTH(REGISTER_MAP); ++i)
    {
        switch (REGISTER_MAP[i].class)
        {
        case ZYDIS_REGCLASS_INVALID:
        case ZYDIS_REGCLASS_FLAGS:
        case ZYDIS_REGCLASS_IP:
            break;
        default:
            if ((reg >= REGISTER_MAP[i].lo) && (reg <= REGISTER_MAP[i].hi))
            {
                return (ZyanU8)(reg - REGISTER_MAP[i].lo);
            }
        }
    }
    return -1;
}

ZydisRegisterClass ZydisRegisterGetClass(ZydisRegister reg)
{
    for (ZyanUSize i = 0; i < ZYAN_ARRAY_LENGTH(REGISTER_MAP); ++i)
    {
        if ((reg >= REGISTER_MAP[i].lo) && (reg <= REGISTER_MAP[i].hi))
        {
            return REGISTER_MAP[i].class;
        }
    }
    return ZYDIS_REGCLASS_INVALID;
}

ZydisRegisterWidth ZydisRegisterGetWidth(ZydisMachineMode mode, ZydisRegister reg)
{
    // Special cases
    switch (reg)
    {
    case ZYDIS_REGISTER_X87CONTROL:
    case ZYDIS_REGISTER_X87STATUS:
    case ZYDIS_REGISTER_X87TAG:
        return 16;
    case ZYDIS_REGISTER_IP:
    case ZYDIS_REGISTER_FLAGS:
        return 16;
    case ZYDIS_REGISTER_EIP:
    case ZYDIS_REGISTER_EFLAGS:
        return 32;
    case ZYDIS_REGISTER_RIP:
    case ZYDIS_REGISTER_RFLAGS:
        return (mode == ZYDIS_MACHINE_MODE_LONG_64) ? 64 : 0;
    case ZYDIS_REGISTER_BNDCFG:
    case ZYDIS_REGISTER_BNDSTATUS:
        return 64;
    case ZYDIS_REGISTER_XCR0:
        return 64;
    case ZYDIS_REGISTER_PKRU:
    case ZYDIS_REGISTER_MXCSR:
        return 32;
    default:
        break;
    }

    // Register classes
    for (ZyanUSize i = 0; i < ZYAN_ARRAY_LENGTH(REGISTER_MAP); ++i)
    {
        if ((reg >= REGISTER_MAP[i].lo) && (reg <= REGISTER_MAP[i].hi))
        {
            return (mode == ZYDIS_MACHINE_MODE_LONG_64) ?
                REGISTER_MAP[i].width64 : REGISTER_MAP[i].width;
        }
    }
    return 0;
}

ZydisRegister ZydisRegisterGetLargestEnclosing(ZydisMachineMode mode,
    ZydisRegister reg)
{
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
        /* R15B */ 15,
    };

    for (ZyanUSize i = 0; i < ZYAN_ARRAY_LENGTH(REGISTER_MAP); ++i)
    {
        if ((reg >= REGISTER_MAP[i].lo) && (reg <= REGISTER_MAP[i].hi))
        {
            const ZydisRegisterClass reg_class = REGISTER_MAP[i].class;
            if ((reg_class == ZYDIS_REGCLASS_GPR64) && (mode != ZYDIS_MACHINE_MODE_LONG_64))
            {
                return ZYDIS_REGISTER_NONE;
            }

            ZyanU8 reg_id = (ZyanU8)(reg - REGISTER_MAP[reg_class].lo);
            switch (reg_class)
            {
            case ZYDIS_REGCLASS_GPR8:
                reg_id = GPR8_MAPPING[reg_id];
                ZYAN_FALLTHROUGH;
            case ZYDIS_REGCLASS_GPR16:
            case ZYDIS_REGCLASS_GPR32:
            case ZYDIS_REGCLASS_GPR64:
                switch (mode)
                {
                case ZYDIS_MACHINE_MODE_LONG_64:
                    return REGISTER_MAP[ZYDIS_REGCLASS_GPR64].lo + reg_id;
                case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
                case ZYDIS_MACHINE_MODE_LEGACY_32:
                    return REGISTER_MAP[ZYDIS_REGCLASS_GPR32].lo + reg_id;
                case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
                case ZYDIS_MACHINE_MODE_LEGACY_16:
                case ZYDIS_MACHINE_MODE_REAL_16:
                    return REGISTER_MAP[ZYDIS_REGCLASS_GPR16].lo + reg_id;
                default:
                    return ZYDIS_REGISTER_NONE;
                }
            case ZYDIS_REGCLASS_XMM:
            case ZYDIS_REGCLASS_YMM:
            case ZYDIS_REGCLASS_ZMM:
#if defined(ZYDIS_DISABLE_AVX512) && defined(ZYDIS_DISABLE_KNC)
                return REGISTER_MAP[ZYDIS_REGCLASS_YMM].lo + reg_id;
#else
                return REGISTER_MAP[ZYDIS_REGCLASS_ZMM].lo + reg_id;
#endif
            default:
                return ZYDIS_REGISTER_NONE;
            }
        }
    }

    return ZYDIS_REGISTER_NONE;
}

const char* ZydisRegisterGetString(ZydisRegister reg)
{
    if ((ZyanUSize)reg >= ZYAN_ARRAY_LENGTH(STR_REGISTER))
    {
        return ZYAN_NULL;
    }
    return STR_REGISTER[reg].data;
}

const ZydisShortString* ZydisRegisterGetStringWrapped(ZydisRegister reg)
{
    if ((ZyanUSize)reg >= ZYAN_ARRAY_LENGTH(STR_REGISTER))
    {
        return ZYAN_NULL;
    }
    return &STR_REGISTER[reg];
}

/* ---------------------------------------------------------------------------------------------- */
/* Register class                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

ZydisRegisterWidth ZydisRegisterClassGetWidth(ZydisMachineMode mode,
    ZydisRegisterClass register_class)
{
    if ((ZyanUSize)register_class < ZYAN_ARRAY_LENGTH(REGISTER_MAP))
    {
        return (mode == ZYDIS_MACHINE_MODE_LONG_64) ?
            REGISTER_MAP[register_class].width64 : REGISTER_MAP[register_class].width;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
