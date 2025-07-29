/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2014-2018 Magnus Edenhill
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

#ifndef _RDKAFKA_CONFVAL_H_
#define _RDKAFKA_CONFVAL_H_
/**
 * @name Next generation configuration values
 * @{
 *
 */

/**
 * @brief Configuration value type
 */
typedef enum rd_kafka_confval_type_t {
        RD_KAFKA_CONFVAL_INT,
        RD_KAFKA_CONFVAL_STR,
        RD_KAFKA_CONFVAL_PTR,
} rd_kafka_confval_type_t;

/**
 * @brief Configuration value (used by AdminOption).
 *        Comes with a type, backed by a union, and a flag to indicate
 *        if the value has been set or not.
 */
typedef struct rd_kafka_confval_s {
        const char *name;                  /**< Property name */
        rd_kafka_confval_type_t valuetype; /**< Value type, maps to union.*/
        int is_set;                        /**< Value has been set. */
        int is_enabled;                    /**< Confval is enabled. */
        union {
                struct {
                        int v;    /**< Current value */
                        int vmin; /**< Minimum value (inclusive) */
                        int vmax; /**< Maximum value (inclusive) */
                        int vdef; /**< Default value */
                } INT;
                struct {
                        char *v;          /**< Current value */
                        int allowempty;   /**< Allow empty string as value */
                        size_t minlen;    /**< Minimum string length excl \0 */
                        size_t maxlen;    /**< Maximum string length excl \0 */
                        const char *vdef; /**< Default value */
                } STR;
                void *PTR; /**< Pointer */
        } u;
} rd_kafka_confval_t;



void rd_kafka_confval_init_int(rd_kafka_confval_t *confval,
                               const char *name,
                               int vmin,
                               int vmax,
                               int vdef);
void rd_kafka_confval_init_ptr(rd_kafka_confval_t *confval, const char *name);
void rd_kafka_confval_disable(rd_kafka_confval_t *confval, const char *name);

rd_kafka_resp_err_t rd_kafka_confval_set_type(rd_kafka_confval_t *confval,
                                              rd_kafka_confval_type_t valuetype,
                                              const void *valuep,
                                              char *errstr,
                                              size_t errstr_size);

int rd_kafka_confval_get_int(const rd_kafka_confval_t *confval);
const char *rd_kafka_confval_get_str(const rd_kafka_confval_t *confval);
void *rd_kafka_confval_get_ptr(const rd_kafka_confval_t *confval);

/**@}*/


#endif /* _RDKAFKA_CONFVAL_H_ */
