/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_MQTT_MQTT3_TO_MQTT5_ADAPTER_SUBSCRIPTION_SET_H
#define AWS_MQTT_MQTT3_TO_MQTT5_ADAPTER_SUBSCRIPTION_SET_H

#include "aws/mqtt/mqtt.h"

#include "aws/mqtt/client.h"
#include "aws/mqtt/v5/mqtt5_types.h"
#include <aws/common/hash_table.h>

/**
 * (Transient) configuration options about a single persistent MQTT topic filter subscription
 */
struct aws_mqtt_subscription_set_subscription_options {
    struct aws_byte_cursor topic_filter;

    enum aws_mqtt5_qos qos;

    bool no_local;
    bool retain_as_published;
    enum aws_mqtt5_retain_handling_type retain_handling_type;

    /* Callback invoked when this subscription matches an incoming publish */
    aws_mqtt_client_publish_received_fn *on_publish_received;

    /* Callback invoked when this subscription is removed from the set */
    aws_mqtt_userdata_cleanup_fn *on_cleanup;

    void *callback_user_data;
};

/**
 * Persistent structure to track a single MQTT topic filter subscription
 */
struct aws_mqtt_subscription_set_subscription_record {
    struct aws_allocator *allocator;
    struct aws_byte_buf topic_filter;

    struct aws_mqtt_subscription_set_subscription_options subscription_view;
};

/**
 * (Transient) configuration options about an incoming publish message
 */
struct aws_mqtt_subscription_set_publish_received_options {
    struct aws_mqtt_client_connection *connection;

    struct aws_byte_cursor topic;
    enum aws_mqtt_qos qos;
    bool retain;
    bool dup;

    struct aws_byte_cursor payload;
};

/**
 * A node in the topic trie maintained by the subscription set.  Each node represents a single "path segment" in a
 * topic filter "path."  Segments can be empty.
 *
 * Some examples (topic filter -> path segments):
 *
 * "hello/world"    -> [ "hello", "world" ]
 * "a/b/"           -> [ "a", "b", "" ]
 * "/b/"             -> [ "", "b", "" ]
 * "a/#/c"          -> [ "a", "#", "c" ]
 *
 * On incoming publish, we walk the tree invoking callbacks based on topic vs. topic filter matching, segment by
 * segment.
 *
 */
struct aws_mqtt_subscription_set_topic_tree_node {
    struct aws_allocator *allocator;

    struct aws_byte_cursor topic_segment_cursor; /* segment can be empty */
    struct aws_byte_buf topic_segment;

    struct aws_mqtt_subscription_set_topic_tree_node *parent;
    struct aws_hash_table children; /* (embedded topic_segment -> containing node) */

    /*
     * A node starts with a ref count of one and is incremented every time a new, overlapping path is added
     * to the subscription set.  When the ref count goes to zero, that means there are not subscriptions using the
     * segment (or path suffix) represented by this node and therefor it can be deleted without any additional
     * analysis.
     *
     * Replacing an existing path does not change the ref count.
     */
    size_t ref_count;

    bool is_subscription;

    aws_mqtt_client_publish_received_fn *on_publish_received;
    aws_mqtt_userdata_cleanup_fn *on_cleanup;

    void *callback_user_data;
};

/**
 * Utility type to track a client's subscriptions.
 *
 * The topic tree supports per-subscription callbacks as used by the MQTT311 implementation.
 *
 * The subscriptions table supports resubscribe APIs for both MQTT311 and MQTT5 by tracking the subscription
 * details on a per-topic-filter basis.
 */
struct aws_mqtt_subscription_set {
    struct aws_allocator *allocator;

    /* a permanent ref */
    struct aws_mqtt_subscription_set_topic_tree_node *root;

    /* embedded topic_filter_cursor -> persistent subscription */
    struct aws_hash_table subscriptions;
};

AWS_EXTERN_C_BEGIN

/**
 * Creates a new subscription set
 *
 * @param allocator allocator to use
 * @return a new subscription set or NULL
 */
AWS_MQTT_API struct aws_mqtt_subscription_set *aws_mqtt_subscription_set_new(struct aws_allocator *allocator);

/**
 * Destroys a subscription set
 *
 * @param subscription_set subscription set to destroy
 */
AWS_MQTT_API void aws_mqtt_subscription_set_destroy(struct aws_mqtt_subscription_set *subscription_set);

/**
 * Checks if a topic filter exists in the subscription set's hash table of subscriptions
 *
 * @param subscription_set subscription set to check
 * @param topic_filter topic filter to check for existence in the set
 * @return true if the topic filter exists in the table of subscriptions, false otherwise
 */
AWS_MQTT_API bool aws_mqtt_subscription_set_is_subscribed(
    const struct aws_mqtt_subscription_set *subscription_set,
    struct aws_byte_cursor topic_filter);

/**
 * Checks if a topic filter exists as a subscription (has a publish received handler) in the set's topic tree
 *
 * @param subscription_set subscription set to check
 * @param topic_filter topic filter to check for existence in the set's topic tree
 * @return true if the set's topic tree contains a publish received callback for the topic filter, false otherwise
 */
AWS_MQTT_API bool aws_mqtt_subscription_set_is_in_topic_tree(
    const struct aws_mqtt_subscription_set *subscription_set,
    struct aws_byte_cursor topic_filter);

/**
 * Adds a subscription to the subscription set.  If a subscription already exists with a matching topic filter, it
 * will be overwritten.
 *
 * @param subscription_set subscription set to add a subscription to
 * @param subscription_options options for the new subscription
 */
AWS_MQTT_API void aws_mqtt_subscription_set_add_subscription(
    struct aws_mqtt_subscription_set *subscription_set,
    const struct aws_mqtt_subscription_set_subscription_options *subscription_options);

/**
 * Removes a subscription from the subscription set
 *
 * @param subscription_set subscription set to remove a subscription from
 * @param topic_filter topic filter to remove subscription information for
 */
AWS_MQTT_API void aws_mqtt_subscription_set_remove_subscription(
    struct aws_mqtt_subscription_set *subscription_set,
    struct aws_byte_cursor topic_filter);

/**
 * Given a publish message, invokes all publish received handlers for matching subscriptions in the subscription set
 *
 * @param subscription_set subscription set to invoke publish received callbacks for
 * @param publish_options received publish message properties
 */
AWS_MQTT_API void aws_mqtt_subscription_set_on_publish_received(
    const struct aws_mqtt_subscription_set *subscription_set,
    const struct aws_mqtt_subscription_set_publish_received_options *publish_options);

/**
 * Queries the properties of all subscriptions tracked by this subscription set.  Used to implement re-subscribe
 * behavior.
 *
 * @param subscription_set subscription set to query the subscriptions on
 * @param subscriptions uninitialized array list to hold the subscriptions.
 *
 * The caller must invoke the cleanup function for array lists on the result.  The list elements are of type
 * 'struct aws_mqtt_subscription_set_subscription_options' and the topic filter cursor points to the subscription set's
 * internal record.  This means that the result must be used and cleaned up in local scope.
 */
AWS_MQTT_API void aws_mqtt_subscription_set_get_subscriptions(
    struct aws_mqtt_subscription_set *subscription_set,
    struct aws_array_list *subscriptions);

/**
 * Creates a new subscription record.  A subscription record tracks all information about a single MQTT topic filter
 * subscription
 *
 * @param allocator memory allocator to use
 * @param subscription all relevant information about the subscription
 * @return a new persistent subscription record
 */
AWS_MQTT_API struct aws_mqtt_subscription_set_subscription_record *aws_mqtt_subscription_set_subscription_record_new(
    struct aws_allocator *allocator,
    const struct aws_mqtt_subscription_set_subscription_options *subscription);

/**
 * Destroys a subscription record
 *
 * @param record subscription record to destroy
 */
AWS_MQTT_API void aws_mqtt_subscription_set_subscription_record_destroy(
    struct aws_mqtt_subscription_set_subscription_record *record);

AWS_EXTERN_C_END

#endif /* AWS_MQTT_MQTT3_TO_MQTT5_ADAPTER_SUBSCRIPTION_SET_H */
