/* Copyright (c) 2023, David Anderson
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

#include <stdlib.h> /* calloc() free() */
#include <string.h> /* memset() strcmp() strncmp() strlen() */

#if defined(_WIN32) && defined(HAVE_STDAFX_H)
#include "stdafx.h"
#endif /* HAVE_STDAFX_H */

#include "dwarf_local_malloc.h"
#include "libdwarf_private.h" /* for TRUE FALSE */
#include "dwarf_secname_ck.h"

/*  Mainly important for Elf */

int
_dwarf_startswith(const char * input,const char* ckfor)
{
    size_t cklen = strlen(ckfor);

    if (! strncmp(input,ckfor,cklen)) {
        return TRUE;
    }
    return FALSE;
}

static int
_dwarf_startswithx(const char * input,const char* ckfor)
{
    size_t cklen =  strlen(ckfor);
    int res = 0;

    res = strncmp(input,ckfor,cklen);
    return res;
}
/*  If one uses the 'sort' command it ignores the proper place
    of . and _ so the output needs modification to get what we want.
    Here ensure _ and . before lowercase letters.
    If any here someday have uppercase ensure _ above
    uppercase letter. */

static const char *nonsec[] = {
".bss",
".comment",
".data",
".fini_array",
".fini",
".got", /* [5] */
".init",
".interp",
".jcr",
".plt",
".rel.data", /* 10 */
".rel.got",
".rel.plt",
".rel.text",
".rela.data",
".rela.got", /* 15 */
".rela.plt",
".rela.text",
".sbss",
".text", /* [19] */
};

/*  These help us ignore some sections that are
    irrelevant to libdwarf.  */
int
_dwarf_ignorethissection(const char *scn_name)
{
    int l = 0;
    int h = sizeof(nonsec)/sizeof(const char *) -1;
    int res = 0;

    while (l <= h) {
        int m = (l+h)/2;
        const char *s = nonsec[m];

        res  = _dwarf_startswithx(scn_name,s);
        if (res < 0) {
            h =  m -1;
        } else if (res > 0) {
            l =  m + 1;
        } else {
            return TRUE;
        }
    }
    return FALSE;
}
