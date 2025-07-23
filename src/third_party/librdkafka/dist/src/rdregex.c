/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2016 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "rd.h"
#include "rdstring.h"
#include "rdregex.h"

#if HAVE_REGEX
#include <regex.h>
struct rd_regex_s {
        regex_t re;
};

#else

#include "regexp.h"
struct rd_regex_s {
        Reprog *re;
};
#endif


/**
 * @brief Destroy compiled regex
 */
void rd_regex_destroy(rd_regex_t *re) {
#if HAVE_REGEX
        regfree(&re->re);
#else
        re_regfree(re->re);
#endif
        rd_free(re);
}


/**
 * @brief Compile regex \p pattern
 * @returns Compiled regex object on success on error.
 */
rd_regex_t *
rd_regex_comp(const char *pattern, char *errstr, size_t errstr_size) {
        rd_regex_t *re = rd_calloc(1, sizeof(*re));
#if HAVE_REGEX
        int r;

        r = regcomp(&re->re, pattern, REG_EXTENDED | REG_NOSUB);
        if (r) {
                if (errstr)
                        regerror(r, &re->re, errstr, errstr_size);
                rd_free(re);
                return NULL;
        }
#else
        const char *errstr2;

        re->re = re_regcomp(pattern, 0, &errstr2);
        if (!re->re) {
                if (errstr)
                        rd_strlcpy(errstr, errstr2, errstr_size);
                rd_free(re);
                return NULL;
        }
#endif

        return re;
}


/**
 * @brief Match \p str to pre-compiled regex \p re
 * @returns 1 on match, else 0
 */
int rd_regex_exec(rd_regex_t *re, const char *str) {
#if HAVE_REGEX
        return regexec(&re->re, str, 0, NULL, 0) != REG_NOMATCH;
#else
        return !re_regexec(re->re, str, NULL, 0);
#endif
}


/**
 * @brief Perform regex match of \p str using regex \p pattern.
 *
 * @returns 1 on match, 0 on non-match or -1 on regex compilation error
 *          in which case a human readable error string is written to
 *          \p errstr (if not NULL).
 */
int rd_regex_match(const char *pattern,
                   const char *str,
                   char *errstr,
                   size_t errstr_size) {
#if HAVE_REGEX /* use libc regex */
        regex_t re;
        int r;

        /* FIXME: cache compiled regex */
        r = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
        if (r) {
                if (errstr)
                        regerror(r, &re, errstr, errstr_size);
                return 0;
        }

        r = regexec(&re, str, 0, NULL, 0) != REG_NOMATCH;

        regfree(&re);

        return r;

#else /* Using regexp.h from minilibs (included) */
        Reprog *re;
        int r;
        const char *errstr2;

        /* FIXME: cache compiled regex */
        re = re_regcomp(pattern, 0, &errstr2);
        if (!re) {
                if (errstr)
                        rd_strlcpy(errstr, errstr2, errstr_size);
                return -1;
        }

        r = !re_regexec(re, str, NULL, 0);

        re_regfree(re);

        return r;
#endif
}
