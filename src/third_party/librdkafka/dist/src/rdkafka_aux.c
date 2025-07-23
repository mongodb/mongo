/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018 Magnus Edenhill
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
#include "rdkafka_aux.h"
#include "rdkafka_error.h"

rd_kafka_resp_err_t
rd_kafka_topic_result_error(const rd_kafka_topic_result_t *topicres) {
        return topicres->err;
}

const char *
rd_kafka_topic_result_error_string(const rd_kafka_topic_result_t *topicres) {
        return topicres->errstr;
}

const char *
rd_kafka_topic_result_name(const rd_kafka_topic_result_t *topicres) {
        return topicres->topic;
}

/**
 * @brief Create new topic_result (single allocation).
 *
 * @param topic Topic string, if topic_size is != -1 it does not have to
 *              be nul-terminated.
 * @param topic_size Size of topic, or -1 to perform automatic strlen()
 * @param err Error code
 * @param errstr Optional error string.
 *
 * All input arguments are copied.
 */

rd_kafka_topic_result_t *rd_kafka_topic_result_new(const char *topic,
                                                   ssize_t topic_size,
                                                   rd_kafka_resp_err_t err,
                                                   const char *errstr) {
        size_t tlen = topic_size != -1 ? (size_t)topic_size : strlen(topic);
        size_t elen = errstr ? strlen(errstr) + 1 : 0;
        rd_kafka_topic_result_t *terr;

        terr = rd_malloc(sizeof(*terr) + tlen + 1 + elen);

        terr->err = err;

        terr->topic = terr->data;
        memcpy(terr->topic, topic, tlen);
        terr->topic[tlen] = '\0';

        if (errstr) {
                terr->errstr = terr->topic + tlen + 1;
                memcpy(terr->errstr, errstr, elen);
        } else {
                terr->errstr = NULL;
        }

        return terr;
}


/**
 * @brief Destroy topic_result
 */
void rd_kafka_topic_result_destroy(rd_kafka_topic_result_t *terr) {
        rd_free(terr);
}

/**
 * @brief Destroy-variant suitable for rd_list free_cb use.
 */
void rd_kafka_topic_result_free(void *ptr) {
        rd_kafka_topic_result_destroy((rd_kafka_topic_result_t *)ptr);
}

const rd_kafka_error_t *
rd_kafka_group_result_error(const rd_kafka_group_result_t *groupres) {
        return groupres->error;
}

const char *
rd_kafka_group_result_name(const rd_kafka_group_result_t *groupres) {
        return groupres->group;
}

const rd_kafka_topic_partition_list_t *
rd_kafka_group_result_partitions(const rd_kafka_group_result_t *groupres) {
        return groupres->partitions;
}

rd_kafka_group_result_t *
rd_kafka_group_result_copy(const rd_kafka_group_result_t *groupres) {
        return rd_kafka_group_result_new(
            groupres->group, -1, groupres->partitions,
            groupres->error ? rd_kafka_error_copy(groupres->error) : NULL);
}

/**
 * @brief Same as rd_kafka_group_result_copy() but suitable for
 *        rd_list_copy(). The \p opaque is ignored.
 */
void *rd_kafka_group_result_copy_opaque(const void *src_groupres,
                                        void *opaque) {
        return rd_kafka_group_result_copy(src_groupres);
}


/**
 * @brief Create new group_result (single allocation).
 *
 * @param group Group string, if group_size is != -1 it does not have to
 *              be nul-terminated.
 * @param group_size Size of group, or -1 to perform automatic strlen()
 * @param error Error object, or NULL on success. Takes ownership of \p error.
 *
 * All input arguments are copied.
 */

rd_kafka_group_result_t *
rd_kafka_group_result_new(const char *group,
                          ssize_t group_size,
                          const rd_kafka_topic_partition_list_t *partitions,
                          rd_kafka_error_t *error) {
        size_t glen = group_size != -1 ? (size_t)group_size : strlen(group);
        rd_kafka_group_result_t *groupres;

        groupres = rd_calloc(1, sizeof(*groupres) + glen + 1);


        groupres->group = groupres->data;
        memcpy(groupres->group, group, glen);
        groupres->group[glen] = '\0';

        if (partitions)
                groupres->partitions =
                    rd_kafka_topic_partition_list_copy(partitions);

        groupres->error = error;

        return groupres;
}


/**
 * @brief Destroy group_result
 */
void rd_kafka_group_result_destroy(rd_kafka_group_result_t *groupres) {
        if (groupres->partitions)
                rd_kafka_topic_partition_list_destroy(groupres->partitions);
        if (groupres->error)
                rd_kafka_error_destroy(groupres->error);
        rd_free(groupres);
}

/**
 * @brief Destroy-variant suitable for rd_list free_cb use.
 */
void rd_kafka_group_result_free(void *ptr) {
        rd_kafka_group_result_destroy((rd_kafka_group_result_t *)ptr);
}


const rd_kafka_error_t *
rd_kafka_acl_result_error(const rd_kafka_acl_result_t *aclres) {
        return aclres->error;
}

/**
 * @brief Allocates and return an acl result, takes ownership of \p error
 *        (unless NULL).
 *
 * @returns The new acl result.
 */
rd_kafka_acl_result_t *rd_kafka_acl_result_new(rd_kafka_error_t *error) {
        rd_kafka_acl_result_t *acl_res;

        acl_res = rd_calloc(1, sizeof(*acl_res));

        acl_res->error = error;

        return acl_res;
}

/**
 * @brief Destroy acl_result
 */
void rd_kafka_acl_result_destroy(rd_kafka_acl_result_t *acl_res) {
        if (acl_res->error)
                rd_kafka_error_destroy(acl_res->error);
        rd_free(acl_res);
}

/**
 * @brief Destroy-variant suitable for rd_list free_cb use.
 */
void rd_kafka_acl_result_free(void *ptr) {
        rd_kafka_acl_result_destroy((rd_kafka_acl_result_t *)ptr);
}


/**
 * @brief Create a new Node object.
 *
 * @param id The node id.
 * @param host The node host.
 * @param port The node port.
 * @param rack_id (optional) The node rack id.
 * @return A new allocated Node object.
 *         Use rd_kafka_Node_destroy() to free when done.
 */
rd_kafka_Node_t *rd_kafka_Node_new(int id,
                                   const char *host,
                                   uint16_t port,
                                   const char *rack_id) {
        rd_kafka_Node_t *ret = rd_calloc(1, sizeof(*ret));
        ret->id              = id;
        ret->port            = port;
        ret->host            = rd_strdup(host);
        if (rack_id != NULL)
                ret->rack_id = rd_strdup(rack_id);
        return ret;
}

/**
 * @brief Copy \p src Node object
 *
 * @param src The Node to copy.
 * @return A new allocated Node object.
 *         Use rd_kafka_Node_destroy() to free when done.
 */
rd_kafka_Node_t *rd_kafka_Node_copy(const rd_kafka_Node_t *src) {
        return rd_kafka_Node_new(src->id, src->host, src->port, src->rack_id);
}

void rd_kafka_Node_destroy(rd_kafka_Node_t *node) {
        rd_free(node->host);
        if (node->rack_id)
                rd_free(node->rack_id);
        rd_free(node);
}

int rd_kafka_Node_id(const rd_kafka_Node_t *node) {
        return node->id;
}

const char *rd_kafka_Node_host(const rd_kafka_Node_t *node) {
        return node->host;
}

uint16_t rd_kafka_Node_port(const rd_kafka_Node_t *node) {
        return node->port;
}
