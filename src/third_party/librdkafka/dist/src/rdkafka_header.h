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

#ifndef _RDKAFKA_HEADER_H
#define _RDKAFKA_HEADER_H



/**
 * @brief The header list (rd_kafka_headers_t) wraps the generic rd_list_t
 *        with additional fields to keep track of the total on-wire size.
 */
struct rd_kafka_headers_s {
        rd_list_t rkhdrs_list;  /**< List of (rd_kafka_header_t *) */
        size_t rkhdrs_ser_size; /**< Total serialized size of headers */
};


/**
 * @brief The header item itself is a single-allocation immutable structure
 *        (rd_kafka_header_t) containing the header name, value and value
 *        length.
 *        Both the header name and header value are nul-terminated for
 *        API convenience.
 *        The header value is a tri-state:
 *         - proper value (considered binary) with length > 0
 *         - empty value with length = 0 (pointer is non-NULL and nul-termd)
 *         - null value with length = 0 (pointer is NULL)
 */
typedef struct rd_kafka_header_s {
        size_t rkhdr_ser_size;   /**< Serialized size */
        size_t rkhdr_value_size; /**< Value length (without nul-term) */
        size_t rkhdr_name_size;  /**< Header name size (w/o nul-term) */
        char *rkhdr_value;       /**< Header value (nul-terminated string but
                                  *   considered binary).
                                  *   Will be NULL for null values, else
                                  *   points to rkhdr_name+.. */
        char rkhdr_name[1];      /**< Header name (nul-terminated string).
                                  *   Followed by allocation for value+nul */
} rd_kafka_header_t;


/**
 * @returns the serialized size for the headers
 */
static RD_INLINE RD_UNUSED size_t
rd_kafka_headers_serialized_size(const rd_kafka_headers_t *hdrs) {
        return hdrs->rkhdrs_ser_size;
}

#endif /* _RDKAFKA_HEADER_H */
