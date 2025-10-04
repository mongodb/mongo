/*
Copyright (c) 2021, David Anderson
All rights reserved.

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
#include <stddef.h> /* NULL size_t */
#include "dwarf_safe_strcpy.h"

/*  An strcpy/strncpy which ensures NUL terminated string
    and never overruns the output.
    inlen is strlen() size of in_s
    outlen is buffer and has to have space
    for the NUL in in_s to avoid truncation.
    So typically outlen == (inlen+1).

    If outlen is 0 it quietly returns.
    If outlen is 1 it assigns a NUL byte to  *out and returns.
    If outlen > 0 then this function always writes
    a trailing NUL byte.

        ISO C 9899:1990 specifies that if outlen has extra space
        that the function zeroes the extra bytes.
        And if outlen is too small no NUL byte is written
        to out.

    This function writes only one NUL byte, the rest of
    the 'out' buffer is untouched. Sometimes our callers
    do not have a narrow bound to outlen, so zeroing
    all the unused bytes is wasteful (hence we do not
    do that here).

    If an input is only partly copied due to limited
    target space this may destroy the
    correctness of a multi-byte-string (UTF-8).
    Note that this will not harm dwarfdump as
    dwarfdump 'sanitizes' all printf output
    in a uri-style. So it only prints true ASCII characters
    in the printable range.

    PRECONDITION:
    The pointer arguments are required to be non-null.
*/
void
_dwarf_safe_strcpy(char *out,
    size_t outlen,
    const char *in_s,
    size_t inlen)
{
    size_t      full_inlen = inlen+1;
    char       *cpo = 0;
    const char *cpi= 0;
    const char *cpiend = 0;

    if (full_inlen >= outlen) {
        if (!outlen) {
            return;
        }
        cpo = out;
        cpi= in_s;
        cpiend = in_s +(outlen-1);
    } else {
        /*  If outlen is very large
            strncpy is very wasteful. */
        cpo = out;
        cpi= in_s;
        cpiend = in_s +inlen;
    }
    for ( ; *cpi && cpi < cpiend ; ++cpo, ++cpi) {
        *cpo = *cpi;
    }
    *cpo = 0;
}
