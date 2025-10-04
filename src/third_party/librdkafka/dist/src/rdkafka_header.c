/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2017-2022, Magnus Edenhill
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
#include "rdkafka_header.h"



#define rd_kafka_header_destroy rd_free

void rd_kafka_headers_destroy(rd_kafka_headers_t *hdrs) {
        rd_list_destroy(&hdrs->rkhdrs_list);
        rd_free(hdrs);
}

rd_kafka_headers_t *rd_kafka_headers_new(size_t initial_count) {
        rd_kafka_headers_t *hdrs;

        hdrs = rd_malloc(sizeof(*hdrs));
        rd_list_init(&hdrs->rkhdrs_list, (int)initial_count,
                     rd_kafka_header_destroy);
        hdrs->rkhdrs_ser_size = 0;

        return hdrs;
}

static void *rd_kafka_header_copy(const void *_src, void *opaque) {
        rd_kafka_headers_t *hdrs     = opaque;
        const rd_kafka_header_t *src = (const rd_kafka_header_t *)_src;

        return (void *)rd_kafka_header_add(
            hdrs, src->rkhdr_name, src->rkhdr_name_size, src->rkhdr_value,
            src->rkhdr_value_size);
}

rd_kafka_headers_t *rd_kafka_headers_copy(const rd_kafka_headers_t *src) {
        rd_kafka_headers_t *dst;

        dst = rd_malloc(sizeof(*dst));
        rd_list_init(&dst->rkhdrs_list, rd_list_cnt(&src->rkhdrs_list),
                     rd_kafka_header_destroy);
        dst->rkhdrs_ser_size = 0; /* Updated by header_copy() */
        rd_list_copy_to(&dst->rkhdrs_list, &src->rkhdrs_list,
                        rd_kafka_header_copy, dst);

        return dst;
}



rd_kafka_resp_err_t rd_kafka_header_add(rd_kafka_headers_t *hdrs,
                                        const char *name,
                                        ssize_t name_size,
                                        const void *value,
                                        ssize_t value_size) {
        rd_kafka_header_t *hdr;
        char varint_NameLen[RD_UVARINT_ENC_SIZEOF(int32_t)];
        char varint_ValueLen[RD_UVARINT_ENC_SIZEOF(int32_t)];

        if (name_size == -1)
                name_size = strlen(name);

        if (value_size == -1)
                value_size = value ? strlen(value) : 0;
        else if (!value)
                value_size = 0;

        hdr = rd_malloc(sizeof(*hdr) + name_size + 1 + value_size + 1);
        hdr->rkhdr_name_size = name_size;
        memcpy((void *)hdr->rkhdr_name, name, name_size);
        hdr->rkhdr_name[name_size] = '\0';

        if (likely(value != NULL)) {
                hdr->rkhdr_value = hdr->rkhdr_name + name_size + 1;
                memcpy((void *)hdr->rkhdr_value, value, value_size);
                hdr->rkhdr_value[value_size] = '\0';
                hdr->rkhdr_value_size        = value_size;
        } else {
                hdr->rkhdr_value      = NULL;
                hdr->rkhdr_value_size = 0;
        }

        rd_list_add(&hdrs->rkhdrs_list, hdr);

        /* Calculate serialized size of header */
        hdr->rkhdr_ser_size = name_size + value_size;
        hdr->rkhdr_ser_size += rd_uvarint_enc_i64(
            varint_NameLen, sizeof(varint_NameLen), name_size);
        hdr->rkhdr_ser_size += rd_uvarint_enc_i64(
            varint_ValueLen, sizeof(varint_ValueLen), value_size);
        hdrs->rkhdrs_ser_size += hdr->rkhdr_ser_size;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief header_t(name) to char * comparator
 */
static int rd_kafka_header_cmp_str(void *_a, void *_b) {
        const rd_kafka_header_t *a = _a;
        const char *b              = _b;

        return strcmp(a->rkhdr_name, b);
}

rd_kafka_resp_err_t rd_kafka_header_remove(rd_kafka_headers_t *hdrs,
                                           const char *name) {
        size_t ser_size = 0;
        rd_kafka_header_t *hdr;
        int i;

        RD_LIST_FOREACH_REVERSE(hdr, &hdrs->rkhdrs_list, i) {
                if (rd_kafka_header_cmp_str(hdr, (void *)name))
                        continue;

                ser_size += hdr->rkhdr_ser_size;
                rd_list_remove_elem(&hdrs->rkhdrs_list, i);
                rd_kafka_header_destroy(hdr);
        }

        if (ser_size == 0)
                return RD_KAFKA_RESP_ERR__NOENT;

        rd_dassert(hdrs->rkhdrs_ser_size >= ser_size);
        hdrs->rkhdrs_ser_size -= ser_size;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t rd_kafka_header_get_last(const rd_kafka_headers_t *hdrs,
                                             const char *name,
                                             const void **valuep,
                                             size_t *sizep) {
        const rd_kafka_header_t *hdr;
        int i;
        size_t name_size = strlen(name);

        RD_LIST_FOREACH_REVERSE(hdr, &hdrs->rkhdrs_list, i) {
                if (hdr->rkhdr_name_size == name_size &&
                    !strcmp(hdr->rkhdr_name, name)) {
                        *valuep = hdr->rkhdr_value;
                        *sizep  = hdr->rkhdr_value_size;
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                }
        }

        return RD_KAFKA_RESP_ERR__NOENT;
}


rd_kafka_resp_err_t rd_kafka_header_get(const rd_kafka_headers_t *hdrs,
                                        size_t idx,
                                        const char *name,
                                        const void **valuep,
                                        size_t *sizep) {
        const rd_kafka_header_t *hdr;
        int i;
        size_t mi        = 0; /* index for matching names */
        size_t name_size = strlen(name);

        RD_LIST_FOREACH(hdr, &hdrs->rkhdrs_list, i) {
                if (hdr->rkhdr_name_size == name_size &&
                    !strcmp(hdr->rkhdr_name, name) && mi++ == idx) {
                        *valuep = hdr->rkhdr_value;
                        *sizep  = hdr->rkhdr_value_size;
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                }
        }

        return RD_KAFKA_RESP_ERR__NOENT;
}


rd_kafka_resp_err_t rd_kafka_header_get_all(const rd_kafka_headers_t *hdrs,
                                            size_t idx,
                                            const char **namep,
                                            const void **valuep,
                                            size_t *sizep) {
        const rd_kafka_header_t *hdr;

        hdr = rd_list_elem(&hdrs->rkhdrs_list, (int)idx);
        if (unlikely(!hdr))
                return RD_KAFKA_RESP_ERR__NOENT;

        *namep  = hdr->rkhdr_name;
        *valuep = hdr->rkhdr_value;
        *sizep  = hdr->rkhdr_value_size;
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


size_t rd_kafka_header_cnt(const rd_kafka_headers_t *hdrs) {
        return (size_t)rd_list_cnt(&hdrs->rkhdrs_list);
}
