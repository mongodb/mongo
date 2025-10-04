#ifndef AWS_MQTT_PRIVATE_TOPIC_TREE_H
#define AWS_MQTT_PRIVATE_TOPIC_TREE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/hash_table.h>

#include <aws/mqtt/private/packets.h>

/** Type of function called when a publish received matches a subscription */
typedef void(aws_mqtt_publish_received_fn)(
    const struct aws_byte_cursor *topic,
    const struct aws_byte_cursor *payload,
    bool dup,
    enum aws_mqtt_qos qos,
    bool retain,
    void *user_data);

/**
 * Function called per subscription when iterating through subscriptions.
 * Return true to continue iteration, or false to stop.
 */
typedef bool(
    aws_mqtt_topic_tree_iterator_fn)(const struct aws_byte_cursor *topic, enum aws_mqtt_qos qos, void *user_data);

struct aws_mqtt_topic_node {

    /* This node's part of the topic filter. If in another node's subtopics, this is the key. */
    struct aws_byte_cursor topic;

    /**
     * aws_byte_cursor -> aws_mqtt_topic_node
     * '#' and '+' are special values in here
     */
    struct aws_hash_table subtopics;

    /* The entire topic filter. If !owns_topic_filter, this topic_filter belongs to someone else. */
    const struct aws_string *topic_filter;
    bool owns_topic_filter;

    /* The following will only be populated if the node IS a subscription */
    /* Max QoS to deliver. */
    enum aws_mqtt_qos qos;
    /* Callback to call on message received */
    aws_mqtt_publish_received_fn *callback;
    aws_mqtt_userdata_cleanup_fn *cleanup;
    void *userdata;
};

struct aws_mqtt_topic_tree {
    struct aws_mqtt_topic_node *root;
    struct aws_allocator *allocator;
};

/**
 * The size of transaction instances.
 * When you initialize an aws_array_list for use as a transaction, pass this as the item size.
 */
extern AWS_MQTT_API size_t aws_mqtt_topic_tree_action_size;

/**
 * Initialize a topic tree with an allocator to later use.
 * Note that calling init allocates root.
 */
AWS_MQTT_API int aws_mqtt_topic_tree_init(struct aws_mqtt_topic_tree *tree, struct aws_allocator *allocator);
/**
 * Cleanup and deallocate an entire topic tree.
 */
AWS_MQTT_API void aws_mqtt_topic_tree_clean_up(struct aws_mqtt_topic_tree *tree);

/**
 * Iterates through all registered subscriptions, and calls iterator.
 *
 * Iterator may return false to stop iterating, or true to continue.
 */
AWS_MQTT_API void aws_mqtt_topic_tree_iterate(
    const struct aws_mqtt_topic_tree *tree,
    aws_mqtt_topic_tree_iterator_fn *iterator,
    void *user_data);

/**
 * Gets the total number of subscriptions in the tree.
 */
AWS_MQTT_API size_t aws_mqtt_topic_tree_get_sub_count(const struct aws_mqtt_topic_tree *tree);

/**
 * Insert a new topic filter into the subscription tree (subscribe).
 *
 * \param[in]  tree         The tree to insert into.
 * \param[in]  transaction  The transaction to add the insert action to.
 *                          Must be initialized with aws_mqtt_topic_tree_action_size as item size.
 * \param[in]  topic_filter The topic filter to subscribe on. May contain wildcards.
 * \param[in]  callback     The callback to call on a publish with a matching topic.
 * \param[in]  connection   The connection object to pass to the callback. This is a void* to support client and server
 *                          connections in the future.
 * \param[in]  userdata     The userdata to pass to callback.
 *
 * \returns AWS_OP_SUCCESS on successful insertion, AWS_OP_ERR with aws_last_error() populated on failure.
 *          If AWS_OP_ERR is returned, aws_mqtt_topic_tree_transaction_rollback should be called to prevent leaks.
 */
AWS_MQTT_API int aws_mqtt_topic_tree_transaction_insert(
    struct aws_mqtt_topic_tree *tree,
    struct aws_array_list *transaction,
    const struct aws_string *topic_filter,
    enum aws_mqtt_qos qos,
    aws_mqtt_publish_received_fn *callback,
    aws_mqtt_userdata_cleanup_fn *cleanup,
    void *userdata);

/**
 * Remove a topic filter from the subscription tree (unsubscribe).
 *
 * \param[in]  tree         The tree to remove from.
 * \param[in]  transaction  The transaction to add the insert action to.
 *                          Must be initialized with aws_mqtt_topic_tree_action_size as item size.
 * \param[in]  topic_filter The filter to remove (must be exactly the same as the topic_filter passed to insert).
 * \param[out] old_userdata The userdata assigned to this subscription will be assigned if not NULL.
 *                          \NOTE once the transaction is committed, old_userdata may be destroyed,
 *                              if a cleanup callback was set on insert.
 *
 * \returns AWS_OP_SUCCESS on successful removal, AWS_OP_ERR with aws_last_error() populated on failure.
 *          If AWS_OP_ERR is returned, aws_mqtt_topic_tree_transaction_rollback should be called to prevent leaks.
 */
AWS_MQTT_API int aws_mqtt_topic_tree_transaction_remove(
    struct aws_mqtt_topic_tree *tree,
    struct aws_array_list *transaction,
    const struct aws_byte_cursor *topic_filter,
    void **old_userdata);

AWS_MQTT_API void aws_mqtt_topic_tree_transaction_commit(
    struct aws_mqtt_topic_tree *tree,
    struct aws_array_list *transaction);

AWS_MQTT_API void aws_mqtt_topic_tree_transaction_roll_back(
    struct aws_mqtt_topic_tree *tree,
    struct aws_array_list *transaction);

/**
 * Insert a new topic filter into the subscription tree (subscribe).
 *
 * \param[in]  tree         The tree to insert into.
 * \param[in]  topic_filter The topic filter to subscribe on. May contain wildcards.
 * \param[in]  callback     The callback to call on a publish with a matching topic.
 * \param[in]  connection   The connection object to pass to the callback. This is a void* to support client and server
 *                          connections in the future.
 * \param[in]  userdata     The userdata to pass to callback.
 *
 * \returns AWS_OP_SUCCESS on successful insertion, AWS_OP_ERR with aws_last_error() populated on failure.
 */
AWS_MQTT_API
int aws_mqtt_topic_tree_insert(
    struct aws_mqtt_topic_tree *tree,
    const struct aws_string *topic_filter,
    enum aws_mqtt_qos qos,
    aws_mqtt_publish_received_fn *callback,
    aws_mqtt_userdata_cleanup_fn *cleanup,
    void *userdata);

AWS_MQTT_API
int aws_mqtt_topic_tree_remove(struct aws_mqtt_topic_tree *tree, const struct aws_byte_cursor *topic_filter);

/**
 * Dispatches a publish packet to all subscriptions matching the publish topic.
 *
 * \param[in] tree  The tree to publish on.
 * \param[in] pub   The publish packet to dispatch. The topic MUST NOT contain wildcards.
 */
void AWS_MQTT_API
    aws_mqtt_topic_tree_publish(const struct aws_mqtt_topic_tree *tree, struct aws_mqtt_packet_publish *pub);

#endif /* AWS_MQTT_PRIVATE_TOPIC_TREE_H */
