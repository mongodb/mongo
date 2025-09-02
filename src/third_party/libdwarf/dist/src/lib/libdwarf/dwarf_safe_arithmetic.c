/*
Copyright (c) 2023, David Anderson All rights reserved.

Redistribution and use in source and binary forms, with
or without modification, are permitted provided that the
following conditions are met:

    Redistributions of source code must retain the above
    copyright notice, this list of conditions and the following
    disclaimer.

    Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following
    disclaimer in the documentation and/or other materials
    provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <config.h>

#include <stdlib.h> /* calloc() free() */
#include <string.h> /* memset() */
#include <stdio.h> /* memset(), printf */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_alloc.h"
#include "dwarf_error.h"
#include "dwarf_safe_arithmetic.h"

#ifndef LLONG_MAX
#define LLONG_MAX    9223372036854775807LL
#endif
#ifndef LLONG_MIN
#define LLONG_MIN    (-LLONG_MAX - 1LL)
#endif
#ifndef ULLONG_MAX
#define ULLONG_MAX   18446744073709551615ULL
#endif

/*  Thanks to David Grayson/
codereview.stackexchange.com/questions/98791/
safe-multiplication-of-two-64-bit-signed-integers
*/
int
_dwarf_int64_mult(Dwarf_Signed x, Dwarf_Signed y,
    Dwarf_Signed * result, Dwarf_Debug dbg,
    Dwarf_Error*error)
{
    if (result) {
        *result = 0;
    }
    if (sizeof(Dwarf_Signed) != 8) {
        _dwarf_error_string(dbg,error,
            DW_DLE_ARITHMETIC_OVERFLOW,
            "DW_DLE_ARITHMETIC_OVERFLOW "
            "Signed 64bit multiply overflow(a)");
        return DW_DLV_ERROR;
    }
    if (x > 0 && y > 0 && x > LLONG_MAX / y)  {
        _dwarf_error_string(dbg,error,
            DW_DLE_ARITHMETIC_OVERFLOW,
            "DW_DLE_ARITHMETIC_OVERFLOW "
            "Signed 64bit multiply overflow(b)");
        return DW_DLV_ERROR;
    }
    if (x < 0 && y > 0 && x < LLONG_MIN / y) {
        _dwarf_error_string(dbg,error,
            DW_DLE_ARITHMETIC_OVERFLOW,
            "DW_DLE_ARITHMETIC_OVERFLOW "
            "Signed 64bit multiply overflow(c)");
        return DW_DLV_ERROR;
    }
    if (x > 0 && y < 0 && y < LLONG_MIN / x) {
        _dwarf_error_string(dbg,error,
            DW_DLE_ARITHMETIC_OVERFLOW,
            "DW_DLE_ARITHMETIC_OVERFLOW "
            "Signed 64bit multiply overflow(d)");
        return DW_DLV_ERROR;
    }
    if (x < 0 && y < 0 &&
        (x <= LLONG_MIN ||
        y <= LLONG_MIN ||
        -x > LLONG_MAX / -y)) {
        _dwarf_error_string(dbg,error,
            DW_DLE_ARITHMETIC_OVERFLOW,
            "DW_DLE_ARITHMETIC_OVERFLOW "
            "Signed 64bit multiply overflow(e)");
        return DW_DLV_ERROR;
    }
    if (result) {
        *result = x * y;
    }
    return DW_DLV_OK;
}

int
_dwarf_uint64_mult(Dwarf_Unsigned x, Dwarf_Unsigned y,
    Dwarf_Unsigned * result, Dwarf_Debug dbg,
    Dwarf_Error *error)
{
    if (y && (x > (ULLONG_MAX/y))) {
        _dwarf_error_string(dbg,error,
            DW_DLE_ARITHMETIC_OVERFLOW,
            "DW_DLE_ARITHMETIC_OVERFLOW "
            "unsigned 64bit multiply overflow");
        return DW_DLV_ERROR;
    }
    if (result) {
        *result = x*y;
    }
    return DW_DLV_OK;
}

#if 0 /* ignoring add check here */
/* See:
https://stackoverflow.com/questions/3944505/
detecting-signed-overflow-in-c-c
*/
int _dwarf_signed_add_check(Dwarf_Signed l, Dwarf_Signed r,
    Dwarf_Signed *sum, Dwarf_Debug dbg,
    Dwarf_Error *error)
{
    if (l >= 0) {
        if ((0x7fffffffffffffffLL - l) < r) {
            _dwarf_error_string(dbg,error,
                DW_DLE_ARITHMETIC_OVERFLOW,
                "DW_DLE_ARITHMETIC_OVERFLOW: "
                "Adding integers l+r (l >= 0) overflows");
            return DW_DLV_ERROR;
        }
    } else {
        if (r < (0x7ffffffffffffffeLL -l))
        {
            _dwarf_error_string(dbg,error,
                DW_DLE_ARITHMETIC_OVERFLOW,
                "DW_DLE_ARITHMETIC_OVERFLOW: "
                "Adding integers l+r (l < 0) overflows");
            return DW_DLV_ERROR;
        }
    }
    if (sum) {
        *sum = l + r;
    }
    return DW_DLV_OK;
}
#endif /* 0 */
