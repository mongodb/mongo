/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 *               2023, Confluent Inc.
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
#ifndef _RDKAFKA_REQUEST_H_
#define _RDKAFKA_REQUEST_H_

#include "rdkafka_cgrp.h"
#include "rdkafka_feature.h"


#define RD_KAFKA_ERR_ACTION_PERMANENT 0x1  /* Permanent error */
#define RD_KAFKA_ERR_ACTION_IGNORE    0x2  /* Error can be ignored */
#define RD_KAFKA_ERR_ACTION_REFRESH   0x4  /* Refresh state (e.g., metadata) */
#define RD_KAFKA_ERR_ACTION_RETRY     0x8  /* Retry request after backoff */
#define RD_KAFKA_ERR_ACTION_INFORM    0x10 /* Inform application about err */
#define RD_KAFKA_ERR_ACTION_SPECIAL                                            \
        0x20 /* Special-purpose, depends on context */
#define RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED 0x40 /* ProduceReq msg status */
#define RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED                             \
        0x80                                    /* ProduceReq msg status */
#define RD_KAFKA_ERR_ACTION_MSG_PERSISTED 0x100 /* ProduceReq msg status */
#define RD_KAFKA_ERR_ACTION_FATAL         0x200 /**< Fatal error */
#define RD_KAFKA_ERR_ACTION_END           0     /* var-arg sentinel */

/** @macro bitmask of the message persistence flags */
#define RD_KAFKA_ERR_ACTION_MSG_FLAGS                                          \
        (RD_KAFKA_ERR_ACTION_MSG_NOT_PERSISTED |                               \
         RD_KAFKA_ERR_ACTION_MSG_POSSIBLY_PERSISTED |                          \
         RD_KAFKA_ERR_ACTION_MSG_PERSISTED)

int rd_kafka_err_action(rd_kafka_broker_t *rkb,
                        rd_kafka_resp_err_t err,
                        const rd_kafka_buf_t *request,
                        ...);


const char *rd_kafka_actions2str(int actions);


typedef enum {
        /** Array end sentinel */
        RD_KAFKA_TOPIC_PARTITION_FIELD_END = 0,
        /** Read/write int32_t for partition */
        RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
        /** Read/write int64_t for offset */
        RD_KAFKA_TOPIC_PARTITION_FIELD_OFFSET,
        /** Read/write int32_t for offset leader_epoch */
        RD_KAFKA_TOPIC_PARTITION_FIELD_EPOCH,
        /** Read/write int32_t for current leader_epoch */
        RD_KAFKA_TOPIC_PARTITION_FIELD_CURRENT_EPOCH,
        /** Read/write int16_t for error code */
        RD_KAFKA_TOPIC_PARTITION_FIELD_ERR,
        /** Read/write timestamp */
        RD_KAFKA_TOPIC_PARTITION_FIELD_TIMESTAMP,
        /** Read/write str for metadata */
        RD_KAFKA_TOPIC_PARTITION_FIELD_METADATA,
        /** Noop, useful for ternary ifs */
        RD_KAFKA_TOPIC_PARTITION_FIELD_NOOP,
} rd_kafka_topic_partition_field_t;

/**
 * @name Current Leader and NodeEndpoints for KIP-951
 *       response triggered metadata updates.
 *
 * @{
 */

typedef struct rd_kafkap_CurrentLeader_s {
        int32_t LeaderId;
        int32_t LeaderEpoch;
} rd_kafkap_CurrentLeader_t;

typedef struct rd_kafkap_NodeEndpoint_s {
        int32_t NodeId;
        rd_kafkap_str_t Host;
        int32_t Port;
        rd_kafkap_str_t Rack;
} rd_kafkap_NodeEndpoint_t;

typedef struct rd_kafkap_NodeEndpoints_s {
        int32_t NodeEndpointCnt;
        rd_kafkap_NodeEndpoint_t *NodeEndpoints;
} rd_kafkap_NodeEndpoints_t;

/**@}*/

/**
 * @name Produce tags
 * @{
 *
 */

typedef struct rd_kafkap_Produce_reply_tags_Partition_s {
        int32_t Partition;
        rd_kafkap_CurrentLeader_t CurrentLeader;
} rd_kafkap_Produce_reply_tags_Partition_t;

typedef struct rd_kafkap_Produce_reply_tags_Topic_s {
        char *TopicName;
        rd_kafkap_Produce_reply_tags_Partition_t Partition;
} rd_kafkap_Produce_reply_tags_Topic_t;

typedef struct rd_kafkap_Produce_reply_tags_s {
        int32_t leader_change_cnt;
        rd_kafkap_NodeEndpoints_t NodeEndpoints;
        rd_kafkap_Produce_reply_tags_Topic_t Topic;
} rd_kafkap_Produce_reply_tags_t;

/**@}*/

/**
 * @name Fetch tags
 * @{
 *
 */

typedef struct rd_kafkap_Fetch_reply_tags_Partition_s {
        int32_t Partition;
        rd_kafkap_CurrentLeader_t CurrentLeader;
} rd_kafkap_Fetch_reply_tags_Partition_t;

typedef struct rd_kafkap_Fetch_reply_tags_Topic_s {
        rd_kafka_Uuid_t TopicId;
        int32_t PartitionCnt;
        rd_kafkap_Fetch_reply_tags_Partition_t *Partitions;
        int32_t partitions_with_leader_change_cnt;
} rd_kafkap_Fetch_reply_tags_Topic_t;

typedef struct rd_kafkap_Fetch_reply_tags_s {
        rd_kafkap_NodeEndpoints_t NodeEndpoints;
        int32_t TopicCnt;
        rd_kafkap_Fetch_reply_tags_Topic_t *Topics;
        int32_t topics_with_leader_change_cnt;
} rd_kafkap_Fetch_reply_tags_t;

/**@}*/

rd_kafka_topic_partition_list_t *rd_kafka_buf_read_topic_partitions(
    rd_kafka_buf_t *rkbuf,
    rd_bool_t use_topic_id,
    rd_bool_t use_topic_name,
    size_t estimated_part_cnt,
    const rd_kafka_topic_partition_field_t *fields);

rd_kafka_topic_partition_list_t *rd_kafka_buf_read_topic_partitions_nullable(
    rd_kafka_buf_t *rkbuf,
    rd_bool_t use_topic_id,
    rd_bool_t use_topic_name,
    size_t estimated_part_cnt,
    const rd_kafka_topic_partition_field_t *fields,
    rd_bool_t *parse_err);

int rd_kafka_buf_write_topic_partitions(
    rd_kafka_buf_t *rkbuf,
    const rd_kafka_topic_partition_list_t *parts,
    rd_bool_t skip_invalid_offsets,
    rd_bool_t only_invalid_offsets,
    rd_bool_t use_topic_id,
    rd_bool_t use_topic_name,
    const rd_kafka_topic_partition_field_t *fields);

int rd_kafka_buf_read_CurrentLeader(rd_kafka_buf_t *rkbuf,
                                    rd_kafkap_CurrentLeader_t *CurrentLeader);

int rd_kafka_buf_read_NodeEndpoints(rd_kafka_buf_t *rkbuf,
                                    rd_kafkap_NodeEndpoints_t *NodeEndpoints);


rd_kafka_resp_err_t
rd_kafka_FindCoordinatorRequest(rd_kafka_broker_t *rkb,
                                rd_kafka_coordtype_t coordtype,
                                const char *coordkey,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque);


rd_kafka_resp_err_t
rd_kafka_handle_ListOffsets(rd_kafka_t *rk,
                            rd_kafka_broker_t *rkb,
                            rd_kafka_resp_err_t err,
                            rd_kafka_buf_t *rkbuf,
                            rd_kafka_buf_t *request,
                            rd_kafka_topic_partition_list_t *offsets,
                            int *actionsp);

void rd_kafka_ListOffsetsRequest(rd_kafka_broker_t *rkb,
                                 rd_kafka_topic_partition_list_t *offsets,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 int timeout_ms,
                                 void *opaque);

rd_kafka_resp_err_t
rd_kafka_ListOffsetsRequest_admin(rd_kafka_broker_t *rkb,
                                  const rd_list_t *offsets,
                                  rd_kafka_AdminOptions_t *options,
                                  char *errstr,
                                  size_t errstr_size,
                                  rd_kafka_replyq_t replyq,
                                  rd_kafka_resp_cb_t *resp_cb,
                                  void *opaque);

rd_kafka_resp_err_t
rd_kafka_parse_ListOffsets(rd_kafka_buf_t *rkbuf,
                           rd_kafka_topic_partition_list_t *offsets,
                           rd_list_t *result_infos);

rd_kafka_resp_err_t
rd_kafka_handle_OffsetForLeaderEpoch(rd_kafka_t *rk,
                                     rd_kafka_broker_t *rkb,
                                     rd_kafka_resp_err_t err,
                                     rd_kafka_buf_t *rkbuf,
                                     rd_kafka_buf_t *request,
                                     rd_kafka_topic_partition_list_t **offsets);
void rd_kafka_OffsetForLeaderEpochRequest(
    rd_kafka_broker_t *rkb,
    rd_kafka_topic_partition_list_t *parts,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque);


rd_kafka_resp_err_t
rd_kafka_handle_OffsetFetch(rd_kafka_t *rk,
                            rd_kafka_broker_t *rkb,
                            rd_kafka_resp_err_t err,
                            rd_kafka_buf_t *rkbuf,
                            rd_kafka_buf_t *request,
                            rd_kafka_topic_partition_list_t **offsets,
                            rd_bool_t update_toppar,
                            rd_bool_t add_part,
                            rd_bool_t allow_retry);

void rd_kafka_op_handle_OffsetFetch(rd_kafka_t *rk,
                                    rd_kafka_broker_t *rkb,
                                    rd_kafka_resp_err_t err,
                                    rd_kafka_buf_t *rkbuf,
                                    rd_kafka_buf_t *request,
                                    void *opaque);

void rd_kafka_OffsetFetchRequest(rd_kafka_broker_t *rkb,
                                 const char *group_id,
                                 rd_kafka_topic_partition_list_t *parts,
                                 rd_bool_t use_topic_id,
                                 int32_t generation_id_or_member_epoch,
                                 rd_kafkap_str_t *member_id,
                                 rd_bool_t require_stable_offsets,
                                 int timeout,
                                 rd_kafka_replyq_t replyq,
                                 void (*resp_cb)(rd_kafka_t *,
                                                 rd_kafka_broker_t *,
                                                 rd_kafka_resp_err_t,
                                                 rd_kafka_buf_t *,
                                                 rd_kafka_buf_t *,
                                                 void *),
                                 void *opaque);

rd_kafka_resp_err_t
rd_kafka_handle_OffsetCommit(rd_kafka_t *rk,
                             rd_kafka_broker_t *rkb,
                             rd_kafka_resp_err_t err,
                             rd_kafka_buf_t *rkbuf,
                             rd_kafka_buf_t *request,
                             rd_kafka_topic_partition_list_t *offsets,
                             rd_bool_t ignore_cgrp);

int rd_kafka_OffsetCommitRequest(rd_kafka_broker_t *rkb,
                                 rd_kafka_consumer_group_metadata_t *cgmetadata,
                                 rd_kafka_topic_partition_list_t *offsets,
                                 rd_kafka_replyq_t replyq,
                                 rd_kafka_resp_cb_t *resp_cb,
                                 void *opaque,
                                 const char *reason);

rd_kafka_resp_err_t
rd_kafka_OffsetDeleteRequest(rd_kafka_broker_t *rkb,
                             /** (rd_kafka_DeleteConsumerGroupOffsets_t*) */
                             const rd_list_t *del_grpoffsets,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque);


void rd_kafka_JoinGroupRequest(rd_kafka_broker_t *rkb,
                               const rd_kafkap_str_t *group_id,
                               const rd_kafkap_str_t *member_id,
                               const rd_kafkap_str_t *group_instance_id,
                               const rd_kafkap_str_t *protocol_type,
                               const rd_list_t *topics,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque);


void rd_kafka_LeaveGroupRequest(rd_kafka_broker_t *rkb,
                                const char *group_id,
                                const char *member_id,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque);
void rd_kafka_handle_LeaveGroup(rd_kafka_t *rk,
                                rd_kafka_broker_t *rkb,
                                rd_kafka_resp_err_t err,
                                rd_kafka_buf_t *rkbuf,
                                rd_kafka_buf_t *request,
                                void *opaque);

void rd_kafka_SyncGroupRequest(rd_kafka_broker_t *rkb,
                               const rd_kafkap_str_t *group_id,
                               int32_t generation_id,
                               const rd_kafkap_str_t *member_id,
                               const rd_kafkap_str_t *group_instance_id,
                               const rd_kafka_group_member_t *assignments,
                               int assignment_cnt,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque);
void rd_kafka_handle_SyncGroup(rd_kafka_t *rk,
                               rd_kafka_broker_t *rkb,
                               rd_kafka_resp_err_t err,
                               rd_kafka_buf_t *rkbuf,
                               rd_kafka_buf_t *request,
                               void *opaque);

rd_kafka_error_t *rd_kafka_ListGroupsRequest(rd_kafka_broker_t *rkb,
                                             int16_t max_ApiVersion,
                                             const char **states,
                                             size_t states_cnt,
                                             const char **types,
                                             size_t types_cnt,
                                             rd_kafka_replyq_t replyq,
                                             rd_kafka_resp_cb_t *resp_cb,
                                             void *opaque);

rd_kafka_error_t *
rd_kafka_DescribeGroupsRequest(rd_kafka_broker_t *rkb,
                               int16_t max_ApiVersion,
                               char **groups,
                               size_t group_cnt,
                               rd_bool_t include_authorized_operations,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque);


void rd_kafka_HeartbeatRequest(rd_kafka_broker_t *rkb,
                               const rd_kafkap_str_t *group_id,
                               int32_t generation_id,
                               const rd_kafkap_str_t *member_id,
                               const rd_kafkap_str_t *group_instance_id,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque);

void rd_kafka_ConsumerGroupHeartbeatRequest(
    rd_kafka_broker_t *rkb,
    const rd_kafkap_str_t *group_id,
    const rd_kafkap_str_t *member_id,
    int32_t member_epoch,
    const rd_kafkap_str_t *group_instance_id,
    const rd_kafkap_str_t *rack_id,
    int32_t rebalance_timeout_ms,
    const rd_kafka_topic_partition_list_t *subscribed_topics,
    rd_kafkap_str_t *subscribed_topic_regex,
    const rd_kafkap_str_t *remote_assignor,
    const rd_kafka_topic_partition_list_t *current_assignments,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque);

rd_kafka_resp_err_t rd_kafka_MetadataRequest(rd_kafka_broker_t *rkb,
                                             const rd_list_t *topics,
                                             rd_list_t *topic_ids,
                                             const char *reason,
                                             rd_bool_t allow_auto_create_topics,
                                             rd_bool_t cgrp_update,
                                             int32_t cgrp_subscription_version,
                                             rd_bool_t force_racks,
                                             rd_kafka_op_t *rko);

rd_kafka_resp_err_t rd_kafka_MetadataRequest_resp_cb(
    rd_kafka_broker_t *rkb,
    const rd_list_t *topics,
    const rd_list_t *topic_ids,
    const char *reason,
    rd_bool_t allow_auto_create_topics,
    rd_bool_t include_cluster_authorized_operations,
    rd_bool_t include_topic_authorized_operations,
    rd_bool_t cgrp_update,
    int32_t cgrp_subscription_version,
    rd_bool_t force_racks,
    rd_kafka_resp_cb_t *resp_cb,
    rd_kafka_replyq_t replyq,
    rd_bool_t force,
    void *opaque);

rd_kafka_resp_err_t
rd_kafka_handle_ApiVersion(rd_kafka_t *rk,
                           rd_kafka_broker_t *rkb,
                           rd_kafka_resp_err_t err,
                           rd_kafka_buf_t *rkbuf,
                           rd_kafka_buf_t *request,
                           struct rd_kafka_ApiVersion **apis,
                           size_t *api_cnt);
void rd_kafka_ApiVersionRequest(rd_kafka_broker_t *rkb,
                                int16_t ApiVersion,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque);

void rd_kafka_SaslHandshakeRequest(rd_kafka_broker_t *rkb,
                                   const char *mechanism,
                                   rd_kafka_replyq_t replyq,
                                   rd_kafka_resp_cb_t *resp_cb,
                                   void *opaque);

void rd_kafka_handle_SaslAuthenticate(rd_kafka_t *rk,
                                      rd_kafka_broker_t *rkb,
                                      rd_kafka_resp_err_t err,
                                      rd_kafka_buf_t *rkbuf,
                                      rd_kafka_buf_t *request,
                                      void *opaque);

void rd_kafka_SaslAuthenticateRequest(rd_kafka_broker_t *rkb,
                                      const void *buf,
                                      size_t size,
                                      rd_kafka_replyq_t replyq,
                                      rd_kafka_resp_cb_t *resp_cb,
                                      void *opaque);

int rd_kafka_ProduceRequest(rd_kafka_broker_t *rkb,
                            rd_kafka_toppar_t *rktp,
                            const rd_kafka_pid_t pid,
                            uint64_t epoch_base_msgid);

rd_kafka_resp_err_t
rd_kafka_CreateTopicsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *new_topics /*(NewTopic_t*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque);

rd_kafka_resp_err_t
rd_kafka_DeleteTopicsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *del_topics /*(DeleteTopic_t*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque);

rd_kafka_resp_err_t rd_kafka_CreatePartitionsRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *new_parts /*(NewPartitions_t*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque);

rd_kafka_resp_err_t
rd_kafka_AlterConfigsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *configs /*(ConfigResource_t*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque);

rd_kafka_resp_err_t rd_kafka_IncrementalAlterConfigsRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *configs /*(ConfigResource_t*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque);

rd_kafka_resp_err_t rd_kafka_DescribeConfigsRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *configs /*(ConfigResource_t*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque);

rd_kafka_resp_err_t
rd_kafka_DeleteGroupsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *del_groups /*(DeleteGroup_t*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque);

void rd_kafka_handle_InitProducerId(rd_kafka_t *rk,
                                    rd_kafka_broker_t *rkb,
                                    rd_kafka_resp_err_t err,
                                    rd_kafka_buf_t *rkbuf,
                                    rd_kafka_buf_t *request,
                                    void *opaque);

rd_kafka_resp_err_t
rd_kafka_InitProducerIdRequest(rd_kafka_broker_t *rkb,
                               const char *transactional_id,
                               int transaction_timeout_ms,
                               const rd_kafka_pid_t *current_pid,
                               char *errstr,
                               size_t errstr_size,
                               rd_kafka_replyq_t replyq,
                               rd_kafka_resp_cb_t *resp_cb,
                               void *opaque);

rd_kafka_resp_err_t
rd_kafka_AddPartitionsToTxnRequest(rd_kafka_broker_t *rkb,
                                   const char *transactional_id,
                                   rd_kafka_pid_t pid,
                                   const rd_kafka_toppar_tqhead_t *rktps,
                                   char *errstr,
                                   size_t errstr_size,
                                   rd_kafka_replyq_t replyq,
                                   rd_kafka_resp_cb_t *resp_cb,
                                   void *opaque);

void rd_kafka_handle_InitProducerId(rd_kafka_t *rk,
                                    rd_kafka_broker_t *rkb,
                                    rd_kafka_resp_err_t err,
                                    rd_kafka_buf_t *rkbuf,
                                    rd_kafka_buf_t *request,
                                    void *opaque);

rd_kafka_resp_err_t
rd_kafka_AddOffsetsToTxnRequest(rd_kafka_broker_t *rkb,
                                const char *transactional_id,
                                rd_kafka_pid_t pid,
                                const char *group_id,
                                char *errstr,
                                size_t errstr_size,
                                rd_kafka_replyq_t replyq,
                                rd_kafka_resp_cb_t *resp_cb,
                                void *opaque);

rd_kafka_resp_err_t rd_kafka_EndTxnRequest(rd_kafka_broker_t *rkb,
                                           const char *transactional_id,
                                           rd_kafka_pid_t pid,
                                           rd_bool_t committed,
                                           char *errstr,
                                           size_t errstr_size,
                                           rd_kafka_replyq_t replyq,
                                           rd_kafka_resp_cb_t *resp_cb,
                                           void *opaque);

int unittest_request(void);

rd_kafka_resp_err_t
rd_kafka_DeleteRecordsRequest(rd_kafka_broker_t *rkb,
                              /*(rd_topic_partition_list_t*)*/
                              const rd_list_t *offsets_list,
                              rd_kafka_AdminOptions_t *options,
                              char *errstr,
                              size_t errstr_size,
                              rd_kafka_replyq_t replyq,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque);

rd_kafka_resp_err_t
rd_kafka_CreateAclsRequest(rd_kafka_broker_t *rkb,
                           const rd_list_t *new_acls /*(AclBinding_t*)*/,
                           rd_kafka_AdminOptions_t *options,
                           char *errstr,
                           size_t errstr_size,
                           rd_kafka_replyq_t replyq,
                           rd_kafka_resp_cb_t *resp_cb,
                           void *opaque);

rd_kafka_resp_err_t
rd_kafka_DescribeAclsRequest(rd_kafka_broker_t *rkb,
                             const rd_list_t *acls /*(AclBinding*)*/,
                             rd_kafka_AdminOptions_t *options,
                             char *errstr,
                             size_t errstr_size,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque);

rd_kafka_resp_err_t
rd_kafka_DeleteAclsRequest(rd_kafka_broker_t *rkb,
                           const rd_list_t *del_acls /*(AclBindingFilter*)*/,
                           rd_kafka_AdminOptions_t *options,
                           char *errstr,
                           size_t errstr_size,
                           rd_kafka_replyq_t replyq,
                           rd_kafka_resp_cb_t *resp_cb,
                           void *opaque);

rd_kafka_resp_err_t rd_kafka_ElectLeadersRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *elect_leaders /*(rd_kafka_EleactLeaders_t*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque);

rd_kafka_error_t *
rd_kafka_ConsumerGroupDescribeRequest(rd_kafka_broker_t *rkb,
                                      char **groups,
                                      size_t group_cnt,
                                      rd_bool_t include_authorized_operations,
                                      rd_kafka_replyq_t replyq,
                                      rd_kafka_resp_cb_t *resp_cb,
                                      void *opaque);

void rd_kafkap_leader_discovery_tmpabuf_add_alloc_brokers(
    rd_tmpabuf_t *tbuf,
    rd_kafkap_NodeEndpoints_t *NodeEndpoints);

void rd_kafkap_leader_discovery_tmpabuf_add_alloc_topics(rd_tmpabuf_t *tbuf,
                                                         int topic_cnt);

void rd_kafkap_leader_discovery_tmpabuf_add_alloc_topic(rd_tmpabuf_t *tbuf,
                                                        char *topic_name,
                                                        int32_t partition_cnt);

void rd_kafkap_leader_discovery_metadata_init(rd_kafka_metadata_internal_t *mdi,
                                              int32_t broker_id);

void rd_kafkap_leader_discovery_set_brokers(
    rd_tmpabuf_t *tbuf,
    rd_kafka_metadata_internal_t *mdi,
    rd_kafkap_NodeEndpoints_t *NodeEndpoints);

void rd_kafkap_leader_discovery_set_topic_cnt(rd_tmpabuf_t *tbuf,
                                              rd_kafka_metadata_internal_t *mdi,
                                              int topic_cnt);

void rd_kafkap_leader_discovery_set_topic(rd_tmpabuf_t *tbuf,
                                          rd_kafka_metadata_internal_t *mdi,
                                          int topic_idx,
                                          rd_kafka_Uuid_t topic_id,
                                          char *topic_name,
                                          int partition_cnt);

void rd_kafkap_leader_discovery_set_CurrentLeader(
    rd_tmpabuf_t *tbuf,
    rd_kafka_metadata_internal_t *mdi,
    int topic_idx,
    int partition_idx,
    int32_t partition_id,
    rd_kafkap_CurrentLeader_t *CurrentLeader);

rd_kafka_resp_err_t
rd_kafka_GetTelemetrySubscriptionsRequest(rd_kafka_broker_t *rkb,
                                          char *errstr,
                                          size_t errstr_size,
                                          rd_kafka_replyq_t replyq,
                                          rd_kafka_resp_cb_t *resp_cb,
                                          void *opaque);

rd_kafka_resp_err_t
rd_kafka_PushTelemetryRequest(rd_kafka_broker_t *rkb,
                              rd_kafka_Uuid_t *client_instance_id,
                              int32_t subscription_id,
                              rd_bool_t terminating,
                              rd_kafka_compression_t compression_type,
                              const void *metrics,
                              size_t metrics_size,
                              char *errstr,
                              size_t errstr_size,
                              rd_kafka_replyq_t replyq,
                              rd_kafka_resp_cb_t *resp_cb,
                              void *opaque);

void rd_kafka_handle_GetTelemetrySubscriptions(rd_kafka_t *rk,
                                               rd_kafka_broker_t *rkb,
                                               rd_kafka_resp_err_t err,
                                               rd_kafka_buf_t *rkbuf,
                                               rd_kafka_buf_t *request,
                                               void *opaque);

void rd_kafka_handle_PushTelemetry(rd_kafka_t *rk,
                                   rd_kafka_broker_t *rkb,
                                   rd_kafka_resp_err_t err,
                                   rd_kafka_buf_t *rkbuf,
                                   rd_kafka_buf_t *request,
                                   void *opaque);


#endif /* _RDKAFKA_REQUEST_H_ */
