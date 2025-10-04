/*
Copyright (c) 2019-2019, David Anderson
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

/*  A lighly generalized data buffer.
    Works for more than just strings,
    but has features (such as ensuring
    data always has a NUL byte following
    the data area used) most useful for C strings.

    All these return either TRUE (the values altered)
    or FALSE (something went wrong, quite likely
    the caller presented a bad format string for the
    value). Normally a string like
    DWARFSTRINGERR is stuck in the output in case of error.

*/

#include <config.h>

#include <stdlib.h> /* free() malloc() strtol() */
#include <string.h> /* memcpy() strlen() */
#include <stddef.h> /* size_t */

#include "libdwarf_private.h"
#include "dwarf_string.h"

/*  m must be a string, like  "DWARFSTRINGERR..."  for this to work */
#define DWSERR(m) dwarfstring_append_length(data,(m),sizeof(m)-1)

static unsigned long minimumnewlen = 30;

/*  Here we set s_data to a valid pointer to a null byte, though
    the s_size and s_avail are set to zero. */
int
dwarfstring_constructor(struct dwarfstring_s *g)
{
    g->s_data = "";
    g->s_size = 0;
    g->s_avail = 0;
    g->s_malloc = FALSE;
    return TRUE;
}

/*  We only increase the s_data space.
    Only calling dwarfstring_destructor
    eliminates space. */
static int
dwarfstring_add_to(struct dwarfstring_s *g,size_t newlen)
{
    char *b          = 0;
    /*  s_size - s_avail is the string without counting
        the null following the string. So, is  strlen()  */
    size_t lastpos   = g->s_size - g->s_avail;
    size_t malloclen = newlen+1;

    /*  ASSERT: newlen as well as malloclen  are
        greater than g->s_size at both call points */
    if (malloclen < minimumnewlen) {
        malloclen = minimumnewlen;
    }
    /*  Not zeroing the new buffer block. */
    b = malloc(malloclen);
    if (!b) {
        return FALSE;
    }
    if (lastpos > 0) {
        /* Copying the non-null bytes in s_data. */
        memcpy(b,g->s_data,lastpos);
    }
    if (g->s_malloc) {
        free(g->s_data);
        g->s_data = 0;
    }
    g->s_data = b;
    /*  s_data[lastpos] is one past the end of anything
        counted as string
        in s_data at the point of call, and is guaranteed
        to be safe as we increased the size of s_data, we did not
        shrink.  And, too, we add 1 to newlen, always,
        so space for a terminating null byte is guaranteed
        available. */
    g->s_data[lastpos] = 0;
    g->s_size = newlen;
    g->s_avail = newlen - lastpos;
    g->s_malloc = TRUE;
    return TRUE;
}

int
dwarfstring_reset(struct dwarfstring_s *g)
{
    if (!g->s_size) {
        /* In initial condition, nothing to do. */
        return TRUE;
    }
    g->s_avail   = g->s_size;
    g->s_data[0] = 0;
    return TRUE;
}

int
dwarfstring_constructor_fixed(struct dwarfstring_s *g,
    size_t len)
{
    int r = FALSE;

    dwarfstring_constructor(g);
    if (len == 0) {
        return TRUE;
    }
    r = dwarfstring_add_to(g,len);
    if (!r) {
        return FALSE;
    }
    return TRUE;
}

int
dwarfstring_constructor_static(struct dwarfstring_s *g,
    char * space,
    size_t len)
{
    dwarfstring_constructor(g);
    g->s_data =    space;
    g->s_data[0] = 0;
    g->s_size =    len;
    g->s_avail =   len;
    g->s_malloc = FALSE;
    return TRUE;
}

void
dwarfstring_destructor(struct dwarfstring_s *g)
{
    if (g->s_malloc) {
        free(g->s_data);
        g->s_data   = 0;
        g->s_malloc = 0;
    }
    /*  The constructor sets all the fields, most to zero.
        s_data is set to point to static string ""
        s_malloc set to FALSE. */
    dwarfstring_constructor(g);
}

/*  For the case where one wants just the first 'len'
    characters of 'str'. NUL terminator provided
    for you in s_data.
*/
int
dwarfstring_append_length(struct dwarfstring_s *g,char *str,
    size_t slen)
{
    /*  lastpos is the length of characters
        without the null-terminator  we call it strlen */
    size_t lastpos = g->s_size - g->s_avail;
    int r          = 0;

    if (!str  || slen ==0) {
        return TRUE;
    }
    if (slen >= g->s_avail) {
        size_t newlen = 0;

        newlen = g->s_size + slen+2;
        r = dwarfstring_add_to(g,newlen);
        if (!r) {
            /* Unable to resize, dare not do anything. */
            return FALSE;
        }
    }
    memcpy(g->s_data + lastpos,str,slen);
    g->s_avail -= slen;
    /*  Adding string terminating null byte.
        Space is guaranteed available to do this.*/
    g->s_data[g->s_size - g->s_avail] = 0;
    return TRUE;
}

int
dwarfstring_append(struct dwarfstring_s *g,char *str)
{
    size_t dlenszt = 0;

    if (!str) {
        return TRUE;
    }
    dlenszt = strlen(str);
    return dwarfstring_append_length(g,str,dlenszt);
}

char *
dwarfstring_string(struct dwarfstring_s *g)
{
    return g->s_data;
}

size_t
dwarfstring_strlen(struct dwarfstring_s *g)
{
    return g->s_size - g->s_avail;
}

static int
_dwarfstring_append_spaces(dwarfstring *data,
    size_t count)
{
    int res = 0;
    char spacebuf[] = {"                                       "};
    size_t charct = sizeof(spacebuf)-1;
    size_t l = count;

    while (l > charct) {
        res = dwarfstring_append_length(data,spacebuf,charct);
        l -= charct;
        if (res != TRUE) {
            return res;
        }
    }
    /* ASSERT: l > 0 */
    res = dwarfstring_append_length(data,spacebuf,l);
    return res;
}
static int
_dwarfstring_append_zeros(dwarfstring *data, size_t l)
{
    int res = 0;
    static char zeros[] = {
        "0000000000000000000000000000000000000000"};
    size_t charct = sizeof(zeros)-1;

    while (l > charct) {
        res = dwarfstring_append_length(data,zeros,charct);
        l -= charct;
        if (res != TRUE) {
            return res;
        }
    }
    /* ASSERT: l > 0 */
    dwarfstring_append_length(data,zeros,l);
    return res;
}

int dwarfstring_append_printf_s(dwarfstring *data,
    char *format,char *s)
{
    size_t stringlenszt = 0;
    size_t next = 0;
    long val = 0;
    char *endptr = 0;
    const char *numptr = 0;
    /* was %[-]fixedlen.  Zero means no len provided. */
    size_t fixedlen = 0;
    /* was %-, nonzero means left-justify */
    long leftjustify = 0;
    size_t prefixlen = 0;
    int res = 0;

    if (!s) {
        DWSERR("<DWARFSTRINGERR: null string pointer to "
            "dwarfstring_append_printf_s>");
        return FALSE;
    }
    stringlenszt = strlen(s);
    if (!format) {
        DWSERR("<DWARFSTRINGERR: null format pointer to "
            "dwarfstring_append_printf_s>");
        return FALSE;
    }
    while (format[next] && format[next] != '%') {
        ++next;
        ++prefixlen;
    }
    if (prefixlen) {
        dwarfstring_append_length(data,format,prefixlen);
        /*  Fall through whether return value TRUE or FALSE */
    }
    if (format[next] != '%') {
        /*   No % operator found, we are done */
        DWSERR("<DWARFSTRINGERR: no percent passed to "
            "dwarfstring_append_printf_s>");
        return FALSE;
    }
    next++;
    if (!format[next]) {
        DWSERR("<DWARFSTRINGERR: empty percent  to "
            "dwarfstring_append_printf_s>");
        return FALSE;
    }
    if (format[next] == ' ') {
        DWSERR("<DWARFSTRINGERR: empty percent  to "
            "dwarfstring_append_printf_s>");
        return FALSE;
    }

    if (format[next] == '-') {
        leftjustify++;
        next++;
    }
    numptr = format+next;
    val = strtol(numptr,&endptr,10);
    if ( endptr != numptr) {
        fixedlen = val;
    }
    next = (endptr - format);
    if (format[next] != 's') {
        DWSERR("<DWARFSTRINGERR: no percent-s to "
            "dwarfstring_append_printf_s>");
        return FALSE;
    }
    next++;

    if (fixedlen && (stringlenszt >= fixedlen)) {
        /*  Ignore  leftjustify (if any) and the stringlenszt
            as the actual string overrides those. */
        leftjustify = 0;
    }
    if (leftjustify) {

        dwarfstring_append_length(data,s,stringlenszt);
        /*  Ignore return value */
        if (fixedlen) {
            size_t trailingspaces = fixedlen - stringlenszt;

            _dwarfstring_append_spaces(data,trailingspaces);
        }
    } else {
        if (fixedlen && fixedlen < stringlenszt) {
            /*  This lets us have fixedlen < stringlenszt by
                taking all the chars from s*/
            dwarfstring_append_length(data,s,stringlenszt);
            /*  Ignore return value, just keep going */
        } else {
            if (fixedlen) {
                size_t leadingspaces = fixedlen - stringlenszt;
                size_t k = 0;

                for ( ; k < leadingspaces; ++k) {
                    dwarfstring_append_length(data," ",1);
                }
            }
            res = dwarfstring_append_length(data,s,stringlenszt);
            if (res == FALSE) {
                return res;
            }
        }
    }
    if (!format[next]) {
        return TRUE;
    }
    {
        char * startpt = format+next;
        size_t suffixlen = strlen(startpt);

        res = dwarfstring_append_length(data,startpt,suffixlen);
    }
    return res;
}

static char v32m[] = {"-2147483648"};
static char v64m[] = {"-9223372036854775808"};
static char dtable[10] = {
'0','1','2','3','4','5','6','7','8','9'
};
static char xtable[16] = {
'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
};
static char Xtable[16] = {
'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'
};

/*  We deal with formats like:
    %d   %5d %05d %+d %+5d %-5d (and ld and lld too). */
int dwarfstring_append_printf_i(dwarfstring *data,
    char *format,
    dwarfstring_i v)
{
    int res = TRUE;
    size_t next = 0;
    long val = 0;
    char *endptr = 0;
    const char *numptr = 0;
    size_t fixedlen = 0;
    int leadingzero = 0;
    int minuscount = 0; /*left justify */
    int pluscount = 0;
    int lcount = 0;
    int ucount = 0;
    int dcount = 0;
    int xcount = 0;
    int Xcount = 0;
    char *ctable = dtable;
    size_t prefixlen = 0;
    int done = 0;

    if (!format) {
        DWSERR("<DWARFSTRINGERR: null format pointer to "
            "dwarfstring_append_printf_i>");
        return FALSE;
    }
    while (format[next] && format[next] != '%') {
        ++next;
        ++prefixlen;
    }
    dwarfstring_append_length(data,format,prefixlen);
    if (format[next] != '%') {
        /*   No % operator found, we are done */
        DWSERR("<DWARFSTRINGERR: no percent passed to "
            "dwarfstring_append_printf_i>");
        return FALSE;
    }
    next++;
    if (!format[next]) {
        DWSERR("<DWARFSTRINGERR: empty percent  to "
            "dwarfstring_append_printf_i>");
        return FALSE;
    }
    if (format[next] == ' ') {
        DWSERR("<DWARFSTRINGERR: empty percent  to "
            "dwarfstring_append_printf_i>");
        return FALSE;
    }
    if (format[next] == '-') {
        minuscount++;
        next++;
    }
    if (format[next] == '+') {
        pluscount++;
        next++;
    }
    if (format[next] == '-') {
        minuscount++;
        next++;
    }
    if (format[next] == '0') {
        leadingzero = 1;
        next++;
    }
    numptr = format+next;
    val = strtol(numptr,&endptr,10);
    if ( endptr != numptr) {
        fixedlen = val;
    }
    next = (endptr - format);
    /*  Following is lx lu or u or llx llu , we take
        all this to mean 64 bits, */
#ifdef _WIN32
    if (format[next] == 'I') {
        /*lcount++;*/
        next++;
    }
    if (format[next] == '6') {
        /*lcount++;*/
        next++;
    }
    if (format[next] == '4') {
        /*lcount++;*/
        next++;
    }
#endif /* _WIN32 */
    if (format[next] == 'l') {
        lcount++;
        next++;
    }
    if (format[next] == 'l') {
        lcount++;
        next++;
    }
    if (format[next] == 'l') {
        lcount++;
        next++;
    }
    if (format[next] == 'u') {
        ucount++;
        next++;
    }
    if (format[next] == 'd') {
        dcount++;
        next++;
    }
    if (format[next] == 'x') {
        xcount++;
        next++;
    }
    if (format[next] == 'X') {
        Xcount++;
        next++;
    }
    if (format[next] == 's') {
        DWSERR( "<DWARFSTRINGERR: format percent s passed to "
            "dwarfstring_append_printf_i>");
        return FALSE;
    }
    if (xcount || Xcount) {
        /*  Use the printf_u for %x and the like
            just copying the entire format makes
            it easier for coders to understand
            nothing much was done */
        DWSERR("<DWARFSTRINGERR: format %x or %X passed to "
            "dwarfstring_append_printf_i>");
        return FALSE;
    }
    if (!dcount || (lcount >2) ||
        (Xcount+xcount+dcount+ucount) > 1) {
        /* error */
        DWSERR( "<DWARFSTRINGERR: format has too many "
            "percent x/d/u/l passed to "
            "dwarfstring_append_printf_i>");
        return FALSE;
    }
    if (pluscount && minuscount) {
        /* We don't allow  format +- */
        DWSERR("<DWARFSTRINGERR: format disallowed. +- passed to "
            "dwarfstring_append_printf_i>");
        return FALSE;
    }
    {
        char digbuf[36];
        char *digptr = digbuf+sizeof(digbuf) -1;
        size_t digcharlen = 0;
        dwarfstring_i remaining = v;
        int vissigned = 0;
        dwarfstring_i divisor = 10;

        *digptr = 0;
        --digptr;
        if (v < 0) {
            vissigned = 1;
            /*  This test is for twos-complement
                machines and would be better done via
                configure with a compile-time check
                so we do not need a size test at runtime. */
            if (sizeof(v) == 8) {
                dwarfstring_u vm = 0x7fffffffffffffffULL;
                if (vm == (dwarfstring_u)~v) {
                    memcpy(digbuf,v64m,sizeof(v64m));
                    digcharlen = sizeof(v64m)-1;
                    digptr = digbuf;
                    done = 1;
                } else {
                    remaining = -v;
                }
            } else if (sizeof(v) == 4) {
                dwarfstring_u vm = 0x7fffffffL;
                if (vm == (dwarfstring_u)~v) {
                    memcpy(digbuf,v32m,sizeof(v32m));
                    digcharlen = sizeof(v32m)-1;
                    digptr = digbuf;
                    done = 1;
                } else {
                    remaining = -v;
                }
            }else {
                DWSERR("<DWARFSTRINGERR: v passed to "
                    "dwarfstring_append_printf_i "
                    "cannot be handled:integer size>");
                return FALSE;
            }
        }
        if (!done) {
            for ( ;; ) {
                dwarfstring_u dig = 0;

                dig = remaining % divisor;
                remaining /= divisor;
                *digptr = ctable[dig];
                digcharlen++;
                if (!remaining) {
                    break;
                }
                --digptr;
            }
            if (vissigned) { /* could check minuscount instead */
                --digptr;
                digcharlen++;
                *digptr = '-';
            } else if (pluscount) {
                --digptr;
                digcharlen++;
                *digptr = '+';
            } else { /* Fall through */ }
        }
        if (fixedlen > 0) {
            if (fixedlen <= digcharlen) {
                dwarfstring_append_length(data,digptr,digcharlen);
            } else {
                size_t prefixcount = fixedlen - digcharlen;
                if (!leadingzero) {
                    _dwarfstring_append_spaces(data,prefixcount);
                    dwarfstring_append_length(data,digptr,digcharlen);
                } else {
                    if (*digptr == '-') {
                        dwarfstring_append_length(data,"-",1);
                        _dwarfstring_append_zeros(data,prefixcount);
                        digptr++;
                        dwarfstring_append_length(data,digptr,
                            digcharlen-1);
                    } else if (*digptr == '+') {
                        dwarfstring_append_length(data,"+",1);
                        _dwarfstring_append_zeros(data,prefixcount);
                        digptr++;
                        dwarfstring_append_length(data,digptr,
                            digcharlen-1);
                    } else {
                        _dwarfstring_append_zeros(data,prefixcount);
                        dwarfstring_append_length(data,digptr,
                            digcharlen);
                    }
                }
            }
        } else {
            res = dwarfstring_append_length(data,digptr,digcharlen);
        }
    }
    if (format[next]) {
        size_t trailinglen = strlen(format+next);
        res = dwarfstring_append_length(data,format+next,trailinglen);
    }
    return res;
}

#if 0 /* Unused trimleadingzeros */
/*  Counts hex chars. divide by two to get bytes from input
    integer. */
static unsigned
trimleadingzeros(char *ptr,size_t digits,unsigned keepcount)
{
    char *cp = ptr;
    size_t leadzeroscount = 0;
    size_t trimoff = 0;

    for (; *cp; ++cp) {
        if (*cp == '0') {
            leadzeroscount++;
            continue;
        }
    }
    trimoff = keepcount - digits;
    if (trimoff&1) {
        trimoff--;
    }
    return trimoff;
}
#endif /*0*/

/*  With gcc version 5.4.0 20160609  a version using
    const char *formatp instead of format[next]
    and deleting the 'next' variable
    is a few hundredths of a second slower, repeatably.

    We deal with formats like:
    %u   %5u %05u (and ld and lld too).
    %x   %5x %05x (and ld and lld too).  */

int dwarfstring_append_printf_u(dwarfstring *data,
    char *format,
    dwarfstring_u v)
{

    size_t next = 0;
    long val = 0;
    char *endptr = 0;
    const char *numptr = 0;
    size_t fixedlen = 0;
    int leadingzero = 0;
    int lcount = 0;
    int ucount = 0;
    int dcount = 0;
    int xcount = 0;
    int Xcount = 0;
    char *ctable = 0;
    size_t divisor = 0;
    size_t prefixlen = 0;

    if (!format) {
        DWSERR("<DWARFSTRINGERR: null format pointer to "
            "dwarfstring_append_printf_u>");
        return FALSE;
    }
    while (format[next] && format[next] != '%') {
        ++next;
        ++prefixlen;
    }
    dwarfstring_append_length(data,format,prefixlen);
    if (format[next] != '%') {
        /*   No % operator found, we are done */
        DWSERR("<DWARFSTRINGERR: no percent passed to "
            "dwarfstring_append_printf_u>");
        return FALSE;
    }
    next++;
    if (!format[next]) {
        DWSERR("<DWARFSTRINGERR: empty percent  to "
            "dwarfstring_append_printf_u>");
        return FALSE;
    }
    if (format[next] == ' ') {
        DWSERR("<DWARFSTRINGERR: empty percent  to "
            "dwarfstring_append_printf_u>");
        return FALSE;
    }
    if (format[next] == '-') {
        DWSERR("<DWARFSTRINGERR: format - passed to "
            "dwarfstring_append_printf_u "
            "cannot be handled>");
        return FALSE;
    }
    if (format[next] == '0') {
        leadingzero = 1;
        next++;
    }
    numptr = format+next;
    val = strtol(numptr,&endptr,10);
    if ( endptr != numptr) {
        fixedlen = val;
    }
    next = (endptr - format);
    /*  Following is lx lu or u or llx llu , we take
        all this to mean 64 bits, */
#ifdef _WIN32
    if (format[next] == 'I') {
        /*lcount++;*/
        next++;
    }
    if (format[next] == '6') {
        /*lcount++;*/
        next++;
    }
    if (format[next] == '4') {
        /*lcount++;*/
        next++;
    }
#endif /* _WIN32 */
    if (format[next] == 'l') {
        lcount++;
        next++;
    }
    if (format[next] == 'l') {
        lcount++;
        next++;
    }
    if (format[next] == 'l') {
        lcount++;
        next++;
    }
    if (format[next] == 'u') {
        ucount++;
        next++;
    }
    if (format[next] == 'd') {
        dcount++;
        next++;
    }
    if (format[next] == 'x') {
        xcount++;
        next++;
    }
    if (format[next] == 'X') {
        Xcount++;
        next++;
    }
    if (format[next] == 's') {
        DWSERR("<DWARFSTRINGERR: format percent-s passed to "
            "dwarfstring_append_printf_u "
            "cannot be handled>");
        return FALSE;
    }
    if ( (Xcount +xcount+dcount+ucount) > 1) {
        DWSERR("<DWARFSTRINGERR: format  percent -x X d u repeats to "
            "dwarfstring_append_printf_u "
            "cannot be handled>");
        return FALSE;
    }
    if ( (Xcount +xcount+dcount+ucount) == 0) {
        DWSERR("<DWARFSTRINGERR: format percent x X d u missing to "
            "dwarfstring_append_printf_u "
            "cannot be handled>");
        return FALSE;
    }
    if (lcount > 2) {
        DWSERR("<DWARFSTRINGERR: format percent lll to "
            "dwarfstring_append_printf_u "
            "cannot be handled>");
        return FALSE;
    }
    if (dcount > 0) {
        DWSERR("<DWARFSTRINGERR: format  percent-d to "
            "dwarfstring_append_printf_u "
            "cannot be handled>");
        return FALSE;
    }
    if (ucount) {
        divisor = 10;
        ctable = dtable;
    } else {
        divisor = 16;
        if (xcount) {
            ctable = xtable;
        } else {
            ctable = Xtable;
        }
    }
    {
        char digbuf[36];
        char *digptr = 0;
        unsigned digcharlen = 0;
        dwarfstring_u remaining = v;

        if (divisor == 16) {
            digptr = digbuf+sizeof(digbuf) -1;
            for ( ;; ) {
                dwarfstring_u dig;
                dig = remaining & 0xf;
                remaining = remaining >> 4;
                *digptr = ctable[dig];
                ++digcharlen;
                if (!remaining) {
                    break;
                }
                --digptr;
            }
        } else {
            digptr = digbuf+sizeof(digbuf) -1;
            *digptr = 0;
            --digptr;
            for ( ;; ) {
                dwarfstring_u dig;
                dig = remaining % divisor;
                remaining /= divisor;
                *digptr = ctable[dig];
                ++digcharlen;
                if (!remaining) {
                    break;
                }
                --digptr;
            }
        }
        if (fixedlen <= digcharlen) {
            dwarfstring_append_length(data,digptr,digcharlen);
        } else {
            if (!leadingzero) {
                size_t justcount = fixedlen - digcharlen;
                _dwarfstring_append_spaces(data,justcount);
                dwarfstring_append_length(data,digptr,digcharlen);
            } else {
                size_t prefixcount = fixedlen - digcharlen;
                _dwarfstring_append_zeros(data,prefixcount);
                dwarfstring_append_length(data,digptr,digcharlen);
            }
        }
    }
    if (format[next]) {
        size_t trailinglen = strlen(format+next);
        dwarfstring_append_length(data,format+next,trailinglen);
    }
    return FALSE;
}
