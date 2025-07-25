/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2017 Magnus Edenhill
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


#ifndef _RDSTRING_H_
#define _RDSTRING_H_

static RD_INLINE RD_UNUSED void
rd_strlcpy(char *dst, const char *src, size_t dstsize) {
#if HAVE_STRLCPY
        (void)strlcpy(dst, src, dstsize);
#else
        if (likely(dstsize > 0)) {
                size_t srclen  = strlen(src);
                size_t copylen = RD_MIN(srclen, dstsize - 1);
                memcpy(dst, src, copylen);
                dst[copylen] = '\0';
        }
#endif
}



char *rd_string_render(
    const char *templ,
    char *errstr,
    size_t errstr_size,
    ssize_t (*callback)(const char *key, char *buf, size_t size, void *opaque),
    void *opaque);



/**
 * @brief An immutable string tuple (name, value) in a single allocation.
 *        \p value may be NULL.
 */
typedef struct rd_strtup_s {
        char *value;
        char name[1]; /* Actual allocation of name + val here */
} rd_strtup_t;

void rd_strtup_destroy(rd_strtup_t *strtup);
void rd_strtup_free(void *strtup);
rd_strtup_t *rd_strtup_new0(const char *name,
                            ssize_t name_len,
                            const char *value,
                            ssize_t value_len);
rd_strtup_t *rd_strtup_new(const char *name, const char *value);
rd_strtup_t *rd_strtup_dup(const rd_strtup_t *strtup);
void *rd_strtup_list_copy(const void *elem, void *opaque);

char *rd_flags2str(char *dst, size_t size, const char **desc, int flags);

unsigned int rd_string_hash(const char *str, ssize_t len);

int rd_strcmp(const char *a, const char *b);

char *_rd_strcasestr(const char *haystack, const char *needle);

char **rd_string_split(const char *input,
                       char sep,
                       rd_bool_t skip_empty,
                       size_t *cntp);

/** @returns "true" if EXPR is true, else "false" */
#define RD_STR_ToF(EXPR) ((EXPR) ? "true" : "false")

#endif /* _RDSTRING_H_ */
