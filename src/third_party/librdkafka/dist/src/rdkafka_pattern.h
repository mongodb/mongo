/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015 Magnus Edenhill
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
#ifndef _RDKAFKA_PATTERN_H_
#define _RDKAFKA_PATTERN_H_

#include "rdregex.h"

typedef struct rd_kafka_pattern_s {
        TAILQ_ENTRY(rd_kafka_pattern_s) rkpat_link;

        rd_regex_t *rkpat_re; /* Compiled regex */
        char *rkpat_orig;     /* Original pattern */
} rd_kafka_pattern_t;

typedef struct rd_kafka_pattern_list_s {
        TAILQ_HEAD(, rd_kafka_pattern_s) rkpl_head;
        char *rkpl_orig;
} rd_kafka_pattern_list_t;

void rd_kafka_pattern_destroy(rd_kafka_pattern_list_t *plist,
                              rd_kafka_pattern_t *rkpat);
void rd_kafka_pattern_add(rd_kafka_pattern_list_t *plist,
                          rd_kafka_pattern_t *rkpat);
rd_kafka_pattern_t *
rd_kafka_pattern_new(const char *pattern, char *errstr, int errstr_size);
int rd_kafka_pattern_match(rd_kafka_pattern_list_t *plist, const char *str);
int rd_kafka_pattern_list_append(rd_kafka_pattern_list_t *plist,
                                 const char *pattern,
                                 char *errstr,
                                 int errstr_size);
int rd_kafka_pattern_list_remove(rd_kafka_pattern_list_t *plist,
                                 const char *pattern);
void rd_kafka_pattern_list_clear(rd_kafka_pattern_list_t *plist);
void rd_kafka_pattern_list_destroy(rd_kafka_pattern_list_t *plist);
int rd_kafka_pattern_list_init(rd_kafka_pattern_list_t *plist,
                               const char *patternlist,
                               char *errstr,
                               size_t errstr_size);
rd_kafka_pattern_list_t *rd_kafka_pattern_list_new(const char *patternlist,
                                                   char *errstr,
                                                   int errstr_size);
rd_kafka_pattern_list_t *
rd_kafka_pattern_list_copy(rd_kafka_pattern_list_t *src);

#endif /* _RDKAFKA_PATTERN_H_ */
