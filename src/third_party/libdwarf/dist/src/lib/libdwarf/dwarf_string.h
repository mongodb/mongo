/*
Copyright (c) 2019-2023, David Anderson
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

/*  A lightly generalized string buffer for libdwarf.
    The functions that return anything return either
    TRUE (nonzero int) or FALSE (zero)

    On return of FALSE the dwarfstring_s struct
    remains in a usable state.

    It is expected that most users will not check the
    return value.
*/
#ifndef DWARFSTRING_H
#define DWARFSTRING_H
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct dwarfstring_s {
    char *        s_data;
    size_t        s_size;
    size_t        s_avail;
    unsigned char s_malloc;
};

typedef unsigned long long   dwarfstring_u;
typedef signed long long     dwarfstring_i;
typedef struct dwarfstring_s dwarfstring;

int dwarfstring_constructor(struct dwarfstring_s *g);
int dwarfstring_constructor_fixed(struct dwarfstring_s *g,
    size_t len);

/*  When you have an output of a limited length string
    and can allocate a local array to hold it,
    dwarfstring_constructor_static() is good since no malloc
    is used unless the final string length exceeds the buffer
    length.  */
int dwarfstring_constructor_static(struct dwarfstring_s *g,
    char * space, size_t len);
void dwarfstring_destructor(struct dwarfstring_s *g);
int dwarfstring_reset(struct dwarfstring_s *g);

int dwarfstring_append(struct dwarfstring_s *g,char *str);

/*  When one wants the first 'len' characters of str
    appended. NUL termination is provided by dwarfstrings. */
int dwarfstring_append_length(struct dwarfstring_s *g,
    char *str,size_t len);

int dwarfstring_append_printf_s(dwarfstring *data,
    char *format,char *s);
int dwarfstring_append_printf_i(dwarfstring *data,
    char *format,dwarfstring_i);
int dwarfstring_append_printf_u(dwarfstring *data,
    char *format,dwarfstring_u);

char * dwarfstring_string(struct dwarfstring_s *g);
size_t dwarfstring_strlen(struct dwarfstring_s *g);
#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* DWARFSTRING_H */
