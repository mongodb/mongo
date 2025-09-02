/*
  Copyright (C) 2000,2004 Silicon Graphics, Inc.  All Rights Reserved.
  Portions Copyright 2011-2020 David Anderson. All Rights Reserved.

  This program is free software; you can redistribute it
  and/or modify it under the terms of version 2.1 of the
  GNU Lesser General Public License as published by the Free
  Software Foundation.

  This program is distributed in the hope that it would be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.

  Further, this software is distributed without any warranty
  that it is free of the rightful claim of any third person
  regarding infringement or the like.  Any license provided
  herein, whether implied or otherwise, applies only to this
  software file.  Patent licenses, if any, provided herein
  do not apply to combinations of this program with other
  software, or any other product whatsoever.

  You should have received a copy of the GNU Lesser General
  Public License along with this program; if not, write the
  Free Software Foundation, Inc., 51 Franklin Street - Fifth
  Floor, Boston MA 02110-1301, USA.

*/

#include <config.h>

#include <stddef.h> /* size_t */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf.h"
#include "libdwarf.h"
#include "dwarf_local_malloc.h"
#include "libdwarf_private.h"
#include "dwarf_base_types.h"
#include "dwarf_opaque.h"
#include "dwarf_error.h"
#include "dwarf_util.h"

#define MORE_BYTES      0x80
#define DATA_MASK       0x7f
#define DIGIT_WIDTH     7
#define SIGN_BIT        0x40

/*  Note that with 'make check')
    many of the test items
    only make sense if Dwarf_Unsigned (and Dwarf_Signed)
    are 64 bits.  The encode/decode logic should
    be fine whether those types are 64 or 32 bits.
    See runtests.sh */

/* The encode/decode functions here are public */

/*  10 bytes of leb, 7 bits each part of the number, gives
    room for a 64bit number.
    While any number of leading zeroes would be legal, so
    no max is really truly required here, why would a
    compiler generate leading zeros (for unsigned leb)?
    That would seem strange except in rare circumstances
    a compiler may want, for overall alignment, to
    add extra bytes..

    So we allow more than 10 as it is legal for a compiler to
    generate an leb with correct but useless trailing
    zero bytes (note the interaction with sign in the signed case).
    The value of BYTESLEBMAX is arbitrary but allows catching
    corrupt data in a short time.
    Before April 2021 BYTESLEBMAX was 10 as it was assumed
    there were no 'useless' 0x80 high-order bytes in an LEB.
    (0x80 meaning the 7 bits 'value' is zero)
    However in DWARF6 (and at least one compiler before
    DWARF6) a few high order bytes are allowed as padding.

*/
#define BYTESLEBMAX 24
#define BITSPERBYTE 8

/*  When an leb value needs to reveal its length,
    but the value is not needed  */
int
_dwarf_skip_leb128(char * leb128,
    Dwarf_Unsigned * leb128_length,
    char * endptr)
{
    unsigned   byte        = 0;
    /*  The byte_length value will be a small non-negative integer. */
    unsigned   byte_length = 1;

    if (leb128 >=endptr) {
        return DW_DLV_ERROR;
    }

    byte = *(unsigned char *)leb128;
    if ((byte & 0x80) == 0) {
        *leb128_length = 1;
        return DW_DLV_OK;
    } else {
        unsigned       byte2  = 0;
        if ((leb128+1) >=endptr) {
            return DW_DLV_ERROR;
        }
        byte2 = *(unsigned char *)(leb128 + 1);
        if ((byte2 & 0x80) == 0) {
            *leb128_length = 2;
            return DW_DLV_OK;
        }
        /*  Gets messy to hand-inline more byte checking.
            One or two byte leb is very frequent. */
    }

    ++byte_length;
    ++leb128;
    /*  Validity of leb128+1 checked above */
    for (;;byte_length++,leb128++) {
        if (leb128 >= endptr) {
            /*  Off end of available space. */
            return DW_DLV_ERROR;
        }
        byte = *(unsigned char *)leb128;
        if (byte & 0x80) {
            if (byte_length >=  BYTESLEBMAX)  {
                /*  Too long. Not sane length. */
                return DW_DLV_ERROR;
            }
            continue;
        }
        break;
    }
    *leb128_length = byte_length;
    return DW_DLV_OK;
}
/*  Decode ULEB with checking.
    Casting leb128 to (unsigned char *) as
    the signedness of char * is unpredictable in C */
int
dwarf_decode_leb128(char * leb128,
    Dwarf_Unsigned * leb128_length,
    Dwarf_Unsigned *outval,
    char * endptr)
{
    unsigned long  byte        = 0;
    Dwarf_Unsigned word_number = 0;
    Dwarf_Unsigned number      = 0;
    unsigned long  shift       = 0; /* at least 32 bits, even Win32 */
    /*  The byte_length value will be a small non-negative integer. */
    unsigned int   byte_length       = 0;

    if (leb128 >=endptr) {
        return DW_DLV_ERROR;
    }
    /*  The following unrolls-the-loop for the first two bytes and
        unpacks into 32 bits to make this as fast as possible.
        word_number is assumed big enough that the shift has a defined
        result. */
    byte = *(unsigned char *)leb128;
    if ((byte & 0x80) == 0) {
        if (leb128_length) {
            *leb128_length = 1;
        }
        if (outval) {
            *outval = byte;
        }
        return DW_DLV_OK;
    } else {
        unsigned long byte2        = 0;
        if ((leb128+1) >=endptr) {
            return DW_DLV_ERROR;
        }
        byte2 =  *(unsigned char *)(leb128 + 1);
        if ((byte2 & 0x80) == 0) {
            if (leb128_length) {
                *leb128_length = 2;
            }
            word_number =  byte & 0x7f;
            word_number |= (byte2 & 0x7f) << 7;
            if (outval) {
                *outval = word_number;
            }
            return DW_DLV_OK;
        }
        /* Gets messy to hand-inline more byte checking. */
    }

    /*  The rest handles long numbers. Because the 'number'
        may be larger than the default int/unsigned,
        we must cast the 'byte' before
        the shift for the shift to have a defined result. */
    number = 0;
    shift = 0;
    byte_length = 1;
    for (;;) {
        unsigned int b = byte & 0x7f;
        if (shift >= (sizeof(number)*BITSPERBYTE)) {
            /*  Shift is large. Maybe corrupt value,
                maybe some padding high-end byte zeroes
                that we can ignore. */
            if (!b) {
                if (byte_length >= BYTESLEBMAX) {
                    /*  Erroneous input.  */
                    if (leb128_length) {
                        *leb128_length = BYTESLEBMAX;
                    }
                    return DW_DLV_ERROR;
                }
                ++leb128;
                /*  shift cannot overflow as
                    BYTESLEBMAX is not a large value */
                shift += 7;
                if (leb128 >=endptr ) {
                    if (leb128 == endptr && !byte) {
                        /* Meaning zero bits a padding byte */
                        if (leb128_length) {
                            *leb128_length = byte_length;
                        }
                        if (outval) {
                            *outval = number;
                        }
                        return DW_DLV_OK;
                    }
                    return DW_DLV_ERROR;
                }
                ++byte_length;
                byte = *(unsigned char *)leb128;
                continue;
            }
            /*  Too big, corrupt data given the non-zero
                byte content */
            return DW_DLV_ERROR;
        }
        number |= ((Dwarf_Unsigned)b << shift);
        if ((byte & 0x80) == 0) {
            if (leb128_length) {
                *leb128_length = byte_length;
            }
            if (outval) {
                *outval = number;
            }
            return DW_DLV_OK;
        }
        shift += 7;
        byte_length++;
        if (byte_length > BYTESLEBMAX) {
            /*  Erroneous input.  */
            if (leb128_length) {
                *leb128_length = BYTESLEBMAX;
            }
            break;
        }
        ++leb128;
        if (leb128 >= endptr) {
            return DW_DLV_ERROR;
        }
        byte = *(unsigned char *)leb128;
    }
    return DW_DLV_ERROR;
}

/*  Decode SLEB with checking
    Casting leb128 to (unsigned char *) as
    the signedness of char * is unpredictable
    in C */
int
dwarf_decode_signed_leb128(char * leb128,
    Dwarf_Unsigned * leb128_length,
    Dwarf_Signed *outval,char * endptr)
{
    Dwarf_Unsigned byte   = 0;
    unsigned int   b      = 0;
    Dwarf_Signed   number = 0;
    unsigned long  shift  = 0;
    int            sign   = FALSE;
    /*  The byte_length value will be a small non-negative integer. */
    unsigned int   byte_length = 1;

    /*  byte_length being the number of bytes
        of data absorbed so far in
        turning the leb into a Dwarf_Signed. */
    if (!outval) {
        return DW_DLV_ERROR;
    }
    if (leb128 >= endptr) {
        return DW_DLV_ERROR;
    }
    byte   = *(unsigned char *)leb128;
    for (;;) {
        b = byte & 0x7f;
        if (shift >= (sizeof(number)*BITSPERBYTE)) {
            /*  Shift is large. Maybe corrupt value,
                maybe some padding high-end byte zeroes
                that we can ignore (but notice sign bit
                from the last usable byte). */
            sign =  b & 0x40;
            if (!byte || byte == 0x40) {
                /*  The value is complete. */
                break;
            }
            if (b == 0) {
                ++byte_length;
                if (byte_length > BYTESLEBMAX) {
                    /*  Erroneous input.  */
                    if (leb128_length) {
                        *leb128_length = BYTESLEBMAX;
                    }
                    return DW_DLV_ERROR;
                }
                ++leb128;
                /*  shift cannot overflow as
                    BYTESLEBMAX is not a large value */
                shift += 7;
                if (leb128 >= endptr) {
                    return DW_DLV_ERROR;
                }
                byte = *(unsigned char *)leb128;
                continue;
            }
            /*  Too big, corrupt data given the non-zero
                byte content */
            return DW_DLV_ERROR;
        }
        /*  This bit of the last byte indicates sign */
        sign =  b & 0x40;
        number |= ((Dwarf_Unsigned)b) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            break;
        }
        ++leb128;
        if (leb128 >= endptr) {
            return DW_DLV_ERROR;
        }
        byte = *(unsigned char *)leb128;
        byte_length++;
        if (byte_length > BYTESLEBMAX) {
            /*  Erroneous input. */
            if (leb128_length) {
                *leb128_length = BYTESLEBMAX;
            }
            return DW_DLV_ERROR;
        }
    }
    if (sign) {
        /* The following avoids undefined behavior. */
        unsigned int shiftlim = sizeof(Dwarf_Signed) * BITSPERBYTE -1;
        if (shift < shiftlim) {
            Dwarf_Signed y = (Dwarf_Signed)
                (((Dwarf_Unsigned)1) << shift);
            Dwarf_Signed x = -y;
            number |= x;
        } else if (shift == shiftlim) {
            Dwarf_Signed x= (((Dwarf_Unsigned)1) << shift);
            number |= x;
        } else {
            /* trailing zeroes case */
            Dwarf_Signed x= (((Dwarf_Unsigned)1) << shiftlim);
            number |= x;
        }
    }
    if (leb128_length) {
        *leb128_length = byte_length;
    }
    *outval = number;
    return DW_DLV_OK;
}

/*  Encode val as a uleb128. This encodes it as an unsigned
    number.
    Return DW_DLV_ERROR or DW_DLV_OK.
    space to write leb number is provided by caller, with caller
    passing length.
    number of bytes used returned thru nbytes arg.
    This never emits padding, it emits the minimum
    number of bytes that can hold the value. */
int dwarf_encode_leb128(Dwarf_Unsigned val, int *nbytes,
    char *space, int splen)
{
    char *a;
    char *end = space + splen;

    a = space;
    do {
        unsigned char uc;

        if (a >= end) {
            return DW_DLV_ERROR;
        }
        uc = val & DATA_MASK;
        val >>= DIGIT_WIDTH;
        if (val != 0) {
            uc |= MORE_BYTES;
        }
        *a = uc;
        a++;
    } while (val);
    *nbytes = (int)(a - space);
    return DW_DLV_OK;
}

/*  This never emits padding at the end, so it
    says nothing about what such would look like
    for a negative value. */
int dwarf_encode_signed_leb128(Dwarf_Signed value, int *nbytes,
    char *space, int splen)
{
    char *str;
    Dwarf_Signed sign = -(value < 0);
    int more = 1;
    char *end = space + splen;

    str = space;

    do {
        unsigned char byte = value & DATA_MASK;

        value >>= DIGIT_WIDTH;

        if (str >= end) {
            return DW_DLV_ERROR;
        }
        /*  Remaining chunks would just contain the sign
            bit, and this chunk
            has already captured at least one sign bit.  */
        if (value == sign &&
            ((byte & SIGN_BIT) == (sign & SIGN_BIT))) {
            more = 0;
        } else {
            byte |= MORE_BYTES;
        }
        *str = byte;
        str++;
    } while (more);
    *nbytes = (int)(str - space);
    return DW_DLV_OK;
}
