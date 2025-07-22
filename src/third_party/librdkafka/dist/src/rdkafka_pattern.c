/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2015-2022, Magnus Edenhill
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

#include "rdkafka_int.h"
#include "rdkafka_pattern.h"

void rd_kafka_pattern_destroy(rd_kafka_pattern_list_t *plist,
                              rd_kafka_pattern_t *rkpat) {
        TAILQ_REMOVE(&plist->rkpl_head, rkpat, rkpat_link);
        rd_regex_destroy(rkpat->rkpat_re);
        rd_free(rkpat->rkpat_orig);
        rd_free(rkpat);
}

void rd_kafka_pattern_add(rd_kafka_pattern_list_t *plist,
                          rd_kafka_pattern_t *rkpat) {
        TAILQ_INSERT_TAIL(&plist->rkpl_head, rkpat, rkpat_link);
}

rd_kafka_pattern_t *
rd_kafka_pattern_new(const char *pattern, char *errstr, int errstr_size) {
        rd_kafka_pattern_t *rkpat;

        rkpat = rd_calloc(1, sizeof(*rkpat));

        /* Verify and precompile pattern */
        if (!(rkpat->rkpat_re = rd_regex_comp(pattern, errstr, errstr_size))) {
                rd_free(rkpat);
                return NULL;
        }

        rkpat->rkpat_orig = rd_strdup(pattern);

        return rkpat;
}



int rd_kafka_pattern_match(rd_kafka_pattern_list_t *plist, const char *str) {
        rd_kafka_pattern_t *rkpat;

        TAILQ_FOREACH(rkpat, &plist->rkpl_head, rkpat_link) {
                if (rd_regex_exec(rkpat->rkpat_re, str))
                        return 1;
        }

        return 0;
}


/**
 * Append pattern to list.
 */
int rd_kafka_pattern_list_append(rd_kafka_pattern_list_t *plist,
                                 const char *pattern,
                                 char *errstr,
                                 int errstr_size) {
        rd_kafka_pattern_t *rkpat;
        rkpat = rd_kafka_pattern_new(pattern, errstr, errstr_size);
        if (!rkpat)
                return -1;

        rd_kafka_pattern_add(plist, rkpat);
        return 0;
}

/**
 * Remove matching patterns.
 * Returns the number of removed patterns.
 */
int rd_kafka_pattern_list_remove(rd_kafka_pattern_list_t *plist,
                                 const char *pattern) {
        rd_kafka_pattern_t *rkpat, *rkpat_tmp;
        int cnt = 0;

        TAILQ_FOREACH_SAFE(rkpat, &plist->rkpl_head, rkpat_link, rkpat_tmp) {
                if (!strcmp(rkpat->rkpat_orig, pattern)) {
                        rd_kafka_pattern_destroy(plist, rkpat);
                        cnt++;
                }
        }
        return cnt;
}

/**
 * Parse a patternlist and populate a list with it.
 */
static int rd_kafka_pattern_list_parse(rd_kafka_pattern_list_t *plist,
                                       const char *patternlist,
                                       char *errstr,
                                       size_t errstr_size) {
        char *s;
        rd_strdupa(&s, patternlist);

        while (s && *s) {
                char *t = s;
                char re_errstr[256];

                /* Find separator */
                while ((t = strchr(t, ','))) {
                        if (t > s && *(t - 1) == ',') {
                                /* separator was escaped,
                                   remove escape and scan again. */
                                memmove(t - 1, t, strlen(t) + 1);
                                t++;
                        } else {
                                *t = '\0';
                                t++;
                                break;
                        }
                }

                if (rd_kafka_pattern_list_append(plist, s, re_errstr,
                                                 sizeof(re_errstr)) == -1) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to parse pattern \"%s\": "
                                    "%s",
                                    s, re_errstr);
                        rd_kafka_pattern_list_clear(plist);
                        return -1;
                }

                s = t;
        }

        return 0;
}


/**
 * Clear a pattern list.
 */
void rd_kafka_pattern_list_clear(rd_kafka_pattern_list_t *plist) {
        rd_kafka_pattern_t *rkpat;

        while ((rkpat = TAILQ_FIRST(&plist->rkpl_head)))
                rd_kafka_pattern_destroy(plist, rkpat);

        if (plist->rkpl_orig) {
                rd_free(plist->rkpl_orig);
                plist->rkpl_orig = NULL;
        }
}


/**
 * Free a pattern list previously created with list_new()
 */
void rd_kafka_pattern_list_destroy(rd_kafka_pattern_list_t *plist) {
        rd_kafka_pattern_list_clear(plist);
        rd_free(plist);
}

/**
 * Initialize a pattern list, optionally populating it with the
 * comma-separated patterns in 'patternlist'.
 */
int rd_kafka_pattern_list_init(rd_kafka_pattern_list_t *plist,
                               const char *patternlist,
                               char *errstr,
                               size_t errstr_size) {
        TAILQ_INIT(&plist->rkpl_head);
        if (patternlist) {
                if (rd_kafka_pattern_list_parse(plist, patternlist, errstr,
                                                errstr_size) == -1)
                        return -1;
                plist->rkpl_orig = rd_strdup(patternlist);
        } else
                plist->rkpl_orig = NULL;

        return 0;
}


/**
 * Allocate and initialize a new list.
 */
rd_kafka_pattern_list_t *rd_kafka_pattern_list_new(const char *patternlist,
                                                   char *errstr,
                                                   int errstr_size) {
        rd_kafka_pattern_list_t *plist;

        plist = rd_calloc(1, sizeof(*plist));

        if (rd_kafka_pattern_list_init(plist, patternlist, errstr,
                                       errstr_size) == -1) {
                rd_free(plist);
                return NULL;
        }

        return plist;
}


/**
 * Make a copy of a pattern list.
 */
rd_kafka_pattern_list_t *
rd_kafka_pattern_list_copy(rd_kafka_pattern_list_t *src) {
        char errstr[16];
        return rd_kafka_pattern_list_new(src->rkpl_orig, errstr,
                                         sizeof(errstr));
}
