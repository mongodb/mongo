/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018-2022, Magnus Edenhill
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

#ifndef _RDKAFKA_ADMIN_H_
#define _RDKAFKA_ADMIN_H_


#include "rdstring.h"
#include "rdmap.h"
#include "rdkafka_error.h"
#include "rdkafka_confval.h"

#if WITH_SSL && OPENSSL_VERSION_NUMBER >= 0x10101000L
#include <openssl/rand.h>
#endif

#if WITH_SSL
typedef struct rd_kafka_broker_s rd_kafka_broker_t;
extern int rd_kafka_ssl_hmac(rd_kafka_broker_t *rkb,
                             const EVP_MD *evp,
                             const rd_chariov_t *in,
                             const rd_chariov_t *salt,
                             int itcnt,
                             rd_chariov_t *out);
#endif

/**
 * @brief Common AdminOptions type used for all admin APIs.
 *
 * @remark Visit AdminOptions_use() when you change this struct
 *         to make sure it is copied properly.
 */
struct rd_kafka_AdminOptions_s {
        rd_kafka_admin_op_t for_api; /**< Limit allowed options to
                                      *   this API (optional) */

        /* Generic */
        rd_kafka_confval_t request_timeout; /**< I32: Full request timeout,
                                             *        includes looking up leader
                                             *        broker,
                                             *        waiting for req/response,
                                             *        etc. */
        rd_ts_t abs_timeout;                /**< Absolute timeout calculated
                                             *   from .timeout */

        /* Specific for one or more APIs */
        rd_kafka_confval_t operation_timeout; /**< I32: Timeout on broker.
                                               *   Valid for:
                                               *     CreateParititons
                                               *     CreateTopics
                                               *     DeleteRecords
                                               *     DeleteTopics
                                               */
        rd_kafka_confval_t validate_only; /**< BOOL: Only validate (on broker),
                                           *   but don't perform action.
                                           *   Valid for:
                                           *     CreateTopics
                                           *     CreatePartitions
                                           *     AlterConfigs
                                           *     IncrementalAlterConfigs
                                           */

        rd_kafka_confval_t broker; /**< INT: Explicitly override
                                    *        broker id to send
                                    *        requests to.
                                    *   Valid for:
                                    *     all
                                    */

        rd_kafka_confval_t
            require_stable_offsets; /**< BOOL: Whether broker should return
                                     * stable offsets (transaction-committed).
                                     * Valid for:
                                     *     ListConsumerGroupOffsets
                                     */
        rd_kafka_confval_t
            include_authorized_operations; /**< BOOL: Whether broker should
                                            * return authorized operations.
                                            * Valid for:
                                            *     DescribeConsumerGroups
                                            *     DescribeCluster
                                            *     DescribeTopics
                                            */

        rd_kafka_confval_t
            match_consumer_group_states; /**< PTR: list of consumer group states
                                          *   to query for.
                                          *   Valid for: ListConsumerGroups.
                                          */

        rd_kafka_confval_t
            match_consumer_group_types; /**< PTR: list of consumer group types
                                         *   to query for.
                                         *   Valid for: ListConsumerGroups.
                                         */

        rd_kafka_confval_t
            isolation_level; /**< INT:Isolation Level needed for list Offset
                              *   to query for.
                              *   Default Set to
                              * RD_KAFKA_ISOLATION_LEVEL_READ_UNCOMMITTED
                              */

        rd_kafka_confval_t opaque; /**< PTR: Application opaque.
                                    *   Valid for all. */
};


/**
 * @name CreateTopics
 * @{
 */

/**
 * @brief NewTopic type, used with CreateTopics.
 */
struct rd_kafka_NewTopic_s {
        /* Required */
        char *topic;            /**< Topic to be created */
        int num_partitions;     /**< Number of partitions to create */
        int replication_factor; /**< Replication factor */

        /* Optional */
        rd_list_t replicas; /**< Type (rd_list_t (int32_t)):
                             *   Array of replica lists indexed by
                             *   partition, size num_partitions. */
        rd_list_t config;   /**< Type (rd_kafka_ConfigEntry_t *):
                             *   List of configuration entries */
};

/**@}*/


/**
 * @name DeleteTopics
 * @{
 */

/**
 * @brief DeleteTopics result
 */
struct rd_kafka_DeleteTopics_result_s {
        rd_list_t topics; /**< Type (rd_kafka_topic_result_t *) */
};

struct rd_kafka_DeleteTopic_s {
        char *topic;  /**< Points to data */
        char data[1]; /**< The topic name is allocated along with
                       *   the struct here. */
};

/**@}*/



/**
 * @name CreatePartitions
 * @{
 */


/**
 * @brief CreatePartitions result
 */
struct rd_kafka_CreatePartitions_result_s {
        rd_list_t topics; /**< Type (rd_kafka_topic_result_t *) */
};

struct rd_kafka_NewPartitions_s {
        char *topic;      /**< Points to data */
        size_t total_cnt; /**< New total partition count */

        /* Optional */
        rd_list_t replicas; /**< Type (rd_list_t (int32_t)):
                             *   Array of replica lists indexed by
                             *   new partition relative index.
                             *   Size is dynamic since we don't
                             *   know how many partitions are actually
                             *   being added by total_cnt */

        char data[1]; /**< The topic name is allocated along with
                       *   the struct here. */
};

/**@}*/



/**
 * @name ConfigEntry
 * @{
 */

struct rd_kafka_ConfigEntry_s {
        rd_strtup_t *kv; /**< Name/Value pair */

        /* Response */

        /* Attributes: this is a struct for easy copying */
        struct {
                /** Operation type, used for IncrementalAlterConfigs */
                rd_kafka_AlterConfigOpType_t op_type;
                rd_kafka_ConfigSource_t source; /**< Config source */
                rd_bool_t is_readonly;  /**< Value is read-only (on broker) */
                rd_bool_t is_default;   /**< Value is at its default */
                rd_bool_t is_sensitive; /**< Value is sensitive */
                rd_bool_t is_synonym;   /**< Value is synonym */
        } a;

        rd_list_t synonyms; /**< Type (rd_kafka_configEntry *) */
};

/**
 * @brief A cluster ConfigResource constisting of:
 *         - resource type (BROKER, TOPIC)
 *         - configuration property name
 *         - configuration property value
 *
 * https://cwiki.apache.org/confluence/display/KAFKA/KIP-133%3A+Describe+and+Alter+Configs+Admin+APIs
 */
struct rd_kafka_ConfigResource_s {
        rd_kafka_ResourceType_t restype; /**< Resource type */
        char *name;                      /**< Resource name, points to .data*/
        rd_list_t config;                /**< Type (rd_kafka_ConfigEntry_t *):
                                          *   List of config props */

        /* Response */
        rd_kafka_resp_err_t err; /**< Response error code */
        char *errstr;            /**< Response error string */

        char data[1]; /**< The name is allocated along with
                       *   the struct here. */
};



/**@}*/

/**
 * @name AlterConfigs
 * @{
 */



struct rd_kafka_AlterConfigs_result_s {
        rd_list_t resources; /**< Type (rd_kafka_ConfigResource_t *) */
};

struct rd_kafka_IncrementalAlterConfigs_result_s {
        rd_list_t resources; /**< Type (rd_kafka_ConfigResource_t *) */
};

struct rd_kafka_ConfigResource_result_s {
        rd_list_t resources; /**< Type (struct rd_kafka_ConfigResource *):
                              *   List of config resources, sans config
                              *   but with response error values. */
};

/**
 * @brief Resource type specific to config apis.
 */
typedef enum rd_kafka_ConfigResourceType_t {
        RD_KAFKA_CONFIG_RESOURCE_UNKNOWN = 0,
        RD_KAFKA_CONFIG_RESOURCE_TOPIC   = 2,
        RD_KAFKA_CONFIG_RESOURCE_BROKER  = 4,
        RD_KAFKA_CONFIG_RESOURCE_GROUP   = 32,
} rd_kafka_ConfigResourceType_t;

/**
 * @brief Maps `rd_kafka_ResourceType_t` to `rd_kafka_ConfigResourceType_t`
 *        for Config Apis. We are incorrectly using `rd_kafka_ResourceType_t` in
 *        both Config Apis and ACL Apis. So, we need this function to map the
 *        resource type internally to `rd_kafka_ConfigResourceType_t`. Like the
 *        enum value for `GROUP` is 32 in Config Apis, but it is 3 for ACL Apis.
 */
rd_kafka_ConfigResourceType_t
rd_kafka_ResourceType_to_ConfigResourceType(rd_kafka_ResourceType_t restype);

/**
 * @brief Maps `rd_kafka_ConfigResourceType_t` to `rd_kafka_ResourceType_t`
 *        for Config Apis. We are incorrectly using `rd_kafka_ResourceType_t` in
 *        both Config Apis and ACL Apis. So, we need this function to map the
 *        `rd_kafka_ConfigResourceType_t` internally to
 *        `rd_kafka_ResourceType_t`. Like the enum value for `GROUP` is 32 in
 *        Config Apis, but it is 3 for ACL Apis.
 */
rd_kafka_ResourceType_t rd_kafka_ConfigResourceType_to_ResourceType(
    rd_kafka_ConfigResourceType_t config_resource_type);


/**@}*/



/**
 * @name DescribeConfigs
 * @{
 */

struct rd_kafka_DescribeConfigs_result_s {
        rd_list_t configs; /**< Type (rd_kafka_ConfigResource_t *) */
};

/**@}*/


/**
 * @name DeleteGroups
 * @{
 */


struct rd_kafka_DeleteGroup_s {
        char *group;  /**< Points to data */
        char data[1]; /**< The group name is allocated along with
                       *   the struct here. */
};

/**@}*/


/**
 * @name DeleteRecords
 * @{
 */

struct rd_kafka_DeleteRecords_s {
        rd_kafka_topic_partition_list_t *offsets;
};

/**@}*/

/**
 * @name ListConsumerGroupOffsets
 * @{
 */

/**
 * @brief ListConsumerGroupOffsets result
 */
struct rd_kafka_ListConsumerGroupOffsets_result_s {
        rd_list_t groups; /**< Type (rd_kafka_group_result_t *) */
};

struct rd_kafka_ListConsumerGroupOffsets_s {
        char *group_id; /**< Points to data */
        rd_kafka_topic_partition_list_t *partitions;
        char data[1]; /**< The group id is allocated along with
                       *   the struct here. */
};

/**@}*/

/**
 * @name AlterConsumerGroupOffsets
 * @{
 */

/**
 * @brief AlterConsumerGroupOffsets result
 */
struct rd_kafka_AlterConsumerGroupOffsets_result_s {
        rd_list_t groups; /**< Type (rd_kafka_group_result_t *) */
};

struct rd_kafka_AlterConsumerGroupOffsets_s {
        char *group_id; /**< Points to data */
        rd_kafka_topic_partition_list_t *partitions;
        char data[1]; /**< The group id is allocated along with
                       *   the struct here. */
};

/**@}*/

/**
 * @name DeleteConsumerGroupOffsets
 * @{
 */

/**
 * @brief DeleteConsumerGroupOffsets result
 */
struct rd_kafka_DeleteConsumerGroupOffsets_result_s {
        rd_list_t groups; /**< Type (rd_kafka_group_result_t *) */
};

struct rd_kafka_DeleteConsumerGroupOffsets_s {
        char *group; /**< Points to data */
        rd_kafka_topic_partition_list_t *partitions;
        char data[1]; /**< The group name is allocated along with
                       *   the struct here. */
};

/**@}*/

/**
 * @name ListOffsets
 * @{
 */

/**
 * @struct ListOffsets result about a single partition
 */
struct rd_kafka_ListOffsetsResultInfo_s {
        rd_kafka_topic_partition_t *topic_partition;
        int64_t timestamp;
};

rd_kafka_ListOffsetsResultInfo_t *
rd_kafka_ListOffsetsResultInfo_new(rd_kafka_topic_partition_t *rktpar,
                                   rd_ts_t timestamp);
/**@}*/

/**
 * @name CreateAcls
 * @{
 */

/**
 * @brief AclBinding type, used with CreateAcls.
 */
struct rd_kafka_AclBinding_s {
        rd_kafka_ResourceType_t restype; /**< Resource type */
        char *name;                      /**< Resource name, points to .data */
        rd_kafka_ResourcePatternType_t
            resource_pattern_type; /**< Resource pattern type */
        char *principal;           /**< Access Control Entry principal */
        char *host;                /**< Access Control Entry host */
        rd_kafka_AclOperation_t operation; /**< AclOperation enumeration */
        rd_kafka_AclPermissionType_t
            permission_type;     /**< AclPermissionType enumeration */
        rd_kafka_error_t *error; /**< Response error, or NULL on success. */
};
/**@}*/

/**
 * @name DeleteAcls
 * @{
 */

/**
 * @brief DeleteAcls_result type, used with DeleteAcls.
 */
struct rd_kafka_DeleteAcls_result_response_s {
        rd_kafka_error_t *error; /**< Response error object, or NULL */
        rd_list_t matching_acls; /**< Type (rd_kafka_AclBinding_t *) */
};

/**@}*/

/**
 * @name ListConsumerGroups
 * @{
 */

/**
 * @struct ListConsumerGroups result for a single group
 */
struct rd_kafka_ConsumerGroupListing_s {
        char *group_id; /**< Group id */
        /** Is it a simple consumer group? That means empty protocol_type. */
        rd_bool_t is_simple_consumer_group;
        rd_kafka_consumer_group_state_t state; /**< Consumer group state. */
        rd_kafka_consumer_group_type_t type;   /**< Consumer group type. */
};


/**
 * @struct ListConsumerGroups results and errors
 */
struct rd_kafka_ListConsumerGroupsResult_s {
        rd_list_t valid;  /**< List of valid ConsumerGroupListing
                               (rd_kafka_ConsumerGroupListing_t *) */
        rd_list_t errors; /**< List of errors (rd_kafka_error_t *) */
};

/**@}*/

/**
 * @name DescribeConsumerGroups
 * @{
 */

/**
 * @struct Assignment of a consumer group member.
 *
 */
struct rd_kafka_MemberAssignment_s {
        /** Partitions assigned to current member. */
        rd_kafka_topic_partition_list_t *partitions;
};

/**
 * @struct Description of a consumer group member.
 *
 */
struct rd_kafka_MemberDescription_s {
        char *client_id;                        /**< Client id */
        char *consumer_id;                      /**< Consumer id */
        char *group_instance_id;                /**< Group instance id */
        char *host;                             /**< Group member host */
        rd_kafka_MemberAssignment_t assignment; /**< Member assignment */
        rd_kafka_MemberAssignment_t
            *target_assignment; /**< Target assignment. `NULL` for `classic`
                                   protocol */
};

/**
 * @struct DescribeConsumerGroups result
 */
struct rd_kafka_ConsumerGroupDescription_s {
        /** Group id */
        char *group_id;
        /** Is it a simple consumer group? That means empty protocol_type. */
        rd_bool_t is_simple_consumer_group;
        /** List of members.
         *  Type (rd_kafka_MemberDescription_t *): members list */
        rd_list_t members;
        /** Protocol type */
        char *protocol_type;
        /** Partition assignor identifier. */
        char *partition_assignor;
        /** Consumer group state. */
        rd_kafka_consumer_group_state_t state;
        /** Consumer group type. */
        rd_kafka_consumer_group_type_t type;
        /** Consumer group coordinator. */
        rd_kafka_Node_t *coordinator;
        /** Count of operations allowed for topic. -1 indicates operations not
         * requested.*/
        int authorized_operations_cnt;
        /** Operations allowed for topic. May be NULL if operations were not
         * requested */
        rd_kafka_AclOperation_t *authorized_operations;
        /** Group specific error. */
        rd_kafka_error_t *error;
};

/**@}*/

/**
 * @name DescribeTopics
 * @{
 */

/**
 * @brief TopicCollection contains a list of topics.
 *
 */
struct rd_kafka_TopicCollection_s {
        char **topics;     /**< List of topic names. */
        size_t topics_cnt; /**< Count of topic names. */
};

/**
 * @brief TopicPartition result type in DescribeTopics result.
 *
 */
struct rd_kafka_TopicPartitionInfo_s {
        int partition;              /**< Partition id. */
        rd_kafka_Node_t *leader;    /**< Leader of the partition. */
        size_t isr_cnt;             /**< Count of insync replicas. */
        rd_kafka_Node_t **isr;      /**< List of in sync replica nodes. */
        size_t replica_cnt;         /**< Count of partition replicas. */
        rd_kafka_Node_t **replicas; /**< List of replica nodes. */
};

/**
 * @struct DescribeTopics result
 */
struct rd_kafka_TopicDescription_s {
        char *topic;              /**< Topic name */
        rd_kafka_Uuid_t topic_id; /**< Topic Id */
        int partition_cnt;        /**< Number of partitions in \p partitions*/
        rd_bool_t is_internal;    /**< Is the topic is internal to Kafka? */
        rd_kafka_TopicPartitionInfo_t **partitions; /**< Partitions */
        rd_kafka_error_t *error;       /**< Topic error reported by broker */
        int authorized_operations_cnt; /**< Count of operations allowed for
                                        * topic. -1 indicates operations not
                                        * requested. */
        rd_kafka_AclOperation_t
            *authorized_operations; /**< Operations allowed for topic. May be
                                     * NULL if operations were not requested */
};

/**@}*/

/**
 * @name DescribeCluster
 * @{
 */
/**
 * @struct DescribeCluster result - internal type.
 */
typedef struct rd_kafka_ClusterDescription_s {
        char *cluster_id;              /**< Cluster id */
        rd_kafka_Node_t *controller;   /**< Current controller. */
        size_t node_cnt;               /**< Count of brokers in the cluster. */
        rd_kafka_Node_t **nodes;       /**< Brokers in the cluster. */
        int authorized_operations_cnt; /**< Count of operations allowed for
                                        * cluster. -1 indicates operations not
                                        * requested. */
        rd_kafka_AclOperation_t
            *authorized_operations; /**< Operations allowed for cluster. May be
                                     * NULL if operations were not requested */

} rd_kafka_ClusterDescription_t;

/**@}*/

/**
 * @name ElectLeaders
 * @{
 */

/**
 * @struct ElectLeaders request object
 */
struct rd_kafka_ElectLeaders_s {
        rd_kafka_ElectionType_t election_type; /*Election Type*/
        rd_kafka_topic_partition_list_t
            *partitions; /*TopicPartitions for election*/
};

/**
 * @struct ElectLeaders result object
 */
typedef struct rd_kafka_ElectLeadersResult_s {
        rd_list_t partitions; /**< Type (rd_kafka_topic_partition_result_t *) */
} rd_kafka_ElectLeadersResult_t;

/**@}*/

#endif /* _RDKAFKA_ADMIN_H_ */
