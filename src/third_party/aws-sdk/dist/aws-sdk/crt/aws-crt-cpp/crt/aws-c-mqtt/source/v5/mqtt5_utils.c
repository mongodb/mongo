/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/v5/mqtt5_utils.h>

#include <aws/common/byte_buf.h>
#include <aws/common/device_random.h>
#include <inttypes.h>

uint8_t aws_mqtt5_compute_fixed_header_byte1(enum aws_mqtt5_packet_type packet_type, uint8_t flags) {
    return flags | ((uint8_t)packet_type << 4);
}

/* encodes a utf8-string (2 byte length + "MQTT") + the version value (5) */
static uint8_t s_connect_variable_length_header_prefix[7] = {0x00, 0x04, 0x4D, 0x51, 0x54, 0x54, 0x05};

struct aws_byte_cursor g_aws_mqtt5_connect_protocol_cursor = {
    .ptr = &s_connect_variable_length_header_prefix[0],
    .len = AWS_ARRAY_SIZE(s_connect_variable_length_header_prefix),
};

void aws_mqtt5_negotiated_settings_log(
    struct aws_mqtt5_negotiated_settings *negotiated_settings,
    enum aws_log_level level) {

    struct aws_logger *temp_logger = aws_logger_get();
    if (temp_logger == NULL || temp_logger->vtable->get_log_level(temp_logger, AWS_LS_MQTT5_GENERAL) < level) {
        return;
    }

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings maxiumum qos set to %d",
        (void *)negotiated_settings,
        negotiated_settings->maximum_qos);

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings session expiry interval set to %" PRIu32,
        (void *)negotiated_settings,
        negotiated_settings->session_expiry_interval);

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings receive maximum from server set to %" PRIu16,
        (void *)negotiated_settings,
        negotiated_settings->receive_maximum_from_server);

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings maximum packet size to server set to %" PRIu32,
        (void *)negotiated_settings,
        negotiated_settings->maximum_packet_size_to_server);

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings topic alias maximum to server set to %" PRIu16,
        (void *)negotiated_settings,
        negotiated_settings->topic_alias_maximum_to_server);

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings topic alias maximum to client set to %" PRIu16,
        (void *)negotiated_settings,
        negotiated_settings->topic_alias_maximum_to_client);

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings server keep alive set to %" PRIu16,
        (void *)negotiated_settings,
        negotiated_settings->server_keep_alive);

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings retain available set to %s",
        (void *)negotiated_settings,
        negotiated_settings->retain_available ? "true" : "false");

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings wildcard subscriptions available set to %s",
        (void *)negotiated_settings,
        negotiated_settings->wildcard_subscriptions_available ? "true" : "false");

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings subscription identifiers available set to %s",
        (void *)negotiated_settings,
        negotiated_settings->subscription_identifiers_available ? "true" : "false");

    AWS_LOGF(
        level,
        AWS_LS_MQTT5_GENERAL,
        "id=%p: aws_mqtt5_negotiated_settings shared subscriptions available set to %s",
        (void *)negotiated_settings,
        negotiated_settings->shared_subscriptions_available ? "true" : "false");
}

int aws_mqtt5_negotiated_settings_init(
    struct aws_allocator *allocator,
    struct aws_mqtt5_negotiated_settings *negotiated_settings,
    const struct aws_byte_cursor *client_id) {
    if (aws_byte_buf_init(&negotiated_settings->client_id_storage, allocator, client_id->len)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_dynamic(&negotiated_settings->client_id_storage, client_id)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt5_negotiated_settings_copy(
    const struct aws_mqtt5_negotiated_settings *source,
    struct aws_mqtt5_negotiated_settings *dest) {
    aws_mqtt5_negotiated_settings_clean_up(dest);

    *dest = *source;
    AWS_ZERO_STRUCT(dest->client_id_storage);

    if (source->client_id_storage.allocator != NULL) {
        return aws_byte_buf_init_copy_from_cursor(
            &dest->client_id_storage,
            source->client_id_storage.allocator,
            aws_byte_cursor_from_buf(&source->client_id_storage));
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt5_negotiated_settings_apply_client_id(
    struct aws_mqtt5_negotiated_settings *negotiated_settings,
    const struct aws_byte_cursor *client_id) {

    if (negotiated_settings->client_id_storage.len == 0) {
        if (aws_byte_buf_append_dynamic(&negotiated_settings->client_id_storage, client_id)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

void aws_mqtt5_negotiated_settings_clean_up(struct aws_mqtt5_negotiated_settings *negotiated_settings) {
    aws_byte_buf_clean_up(&negotiated_settings->client_id_storage);
}

/** Assign defaults values to negotiated_settings */
void aws_mqtt5_negotiated_settings_reset(
    struct aws_mqtt5_negotiated_settings *negotiated_settings,
    const struct aws_mqtt5_packet_connect_view *packet_connect_view) {
    AWS_PRECONDITION(negotiated_settings != NULL);
    AWS_PRECONDITION(packet_connect_view != NULL);

    /* Properties that may be sent in CONNECT to Server. These should only be sent if Client
       changes them from their default values.
    */
    negotiated_settings->server_keep_alive = packet_connect_view->keep_alive_interval_seconds;
    negotiated_settings->session_expiry_interval = 0;
    negotiated_settings->receive_maximum_from_server = AWS_MQTT5_RECEIVE_MAXIMUM;
    negotiated_settings->maximum_packet_size_to_server = AWS_MQTT5_MAXIMUM_PACKET_SIZE;
    negotiated_settings->topic_alias_maximum_to_client = 0;

    // Default for Client is QoS 1. Server default is 2.
    // This should only be changed if server returns a 0 in the CONNACK
    negotiated_settings->maximum_qos = AWS_MQTT5_QOS_AT_LEAST_ONCE;
    negotiated_settings->topic_alias_maximum_to_server = 0;

    // Default is true for following settings but can be changed by Server on CONNACK
    negotiated_settings->retain_available = true;
    negotiated_settings->wildcard_subscriptions_available = true;
    negotiated_settings->subscription_identifiers_available = true;
    negotiated_settings->shared_subscriptions_available = true;

    negotiated_settings->rejoined_session = false;

    /**
     * Apply user set properties to negotiated_settings
     * NULL pointers indicate user has not set a property and it should remain the default value.
     */

    if (packet_connect_view->session_expiry_interval_seconds != NULL) {
        negotiated_settings->session_expiry_interval = *packet_connect_view->session_expiry_interval_seconds;
    }

    if (packet_connect_view->topic_alias_maximum != NULL) {
        negotiated_settings->topic_alias_maximum_to_client = *packet_connect_view->topic_alias_maximum;
    }
}

void aws_mqtt5_negotiated_settings_apply_connack(
    struct aws_mqtt5_negotiated_settings *negotiated_settings,
    const struct aws_mqtt5_packet_connack_view *connack_data) {
    AWS_PRECONDITION(negotiated_settings != NULL);
    AWS_PRECONDITION(connack_data != NULL);

    /**
     * Reconcile CONNACK set properties with current negotiated_settings values
     * NULL pointers indicate Server has not set a property
     */

    if (connack_data->session_expiry_interval != NULL) {
        negotiated_settings->session_expiry_interval = *connack_data->session_expiry_interval;
    }

    if (connack_data->receive_maximum != NULL) {
        negotiated_settings->receive_maximum_from_server = *connack_data->receive_maximum;
    }

    // NULL = Maximum QoS of 2.
    if (connack_data->maximum_qos != NULL) {
        if (*connack_data->maximum_qos < negotiated_settings->maximum_qos) {
            negotiated_settings->maximum_qos = *connack_data->maximum_qos;
        }
    }

    if (connack_data->retain_available != NULL) {
        negotiated_settings->retain_available = *connack_data->retain_available;
    }

    if (connack_data->maximum_packet_size != NULL) {
        negotiated_settings->maximum_packet_size_to_server = *connack_data->maximum_packet_size;
    }

    // If a value is not sent by Server, the Client must not send any Topic Aliases to the Server.
    if (connack_data->topic_alias_maximum != NULL) {
        negotiated_settings->topic_alias_maximum_to_server = *connack_data->topic_alias_maximum;
    }

    if (connack_data->wildcard_subscriptions_available != NULL) {
        negotiated_settings->wildcard_subscriptions_available = *connack_data->wildcard_subscriptions_available;
    }

    if (connack_data->subscription_identifiers_available != NULL) {
        negotiated_settings->subscription_identifiers_available = *connack_data->subscription_identifiers_available;
    }

    if (connack_data->shared_subscriptions_available != NULL) {
        negotiated_settings->shared_subscriptions_available = *connack_data->shared_subscriptions_available;
    }

    if (connack_data->server_keep_alive != NULL) {
        negotiated_settings->server_keep_alive = *connack_data->server_keep_alive;
    }

    if (connack_data->assigned_client_identifier != NULL) {
        aws_mqtt5_negotiated_settings_apply_client_id(negotiated_settings, connack_data->assigned_client_identifier);
    }

    negotiated_settings->rejoined_session = connack_data->session_present;
}

const char *aws_mqtt5_client_session_behavior_type_to_c_string(
    enum aws_mqtt5_client_session_behavior_type session_behavior) {
    switch (aws_mqtt5_client_session_behavior_type_to_non_default(session_behavior)) {
        case AWS_MQTT5_CSBT_CLEAN:
            return "Clean session always";
        case AWS_MQTT5_CSBT_REJOIN_POST_SUCCESS:
            return "Attempt to resume a session after initial connection success";
        case AWS_MQTT5_CSBT_REJOIN_ALWAYS:
            return "Always attempt to resume a session";
        default:
            return "Unknown session behavior";
    }
}

enum aws_mqtt5_client_session_behavior_type aws_mqtt5_client_session_behavior_type_to_non_default(
    enum aws_mqtt5_client_session_behavior_type session_behavior) {
    if (session_behavior == AWS_MQTT5_CSBT_DEFAULT) {
        return AWS_MQTT5_CSBT_CLEAN;
    }

    return session_behavior;
}

const char *aws_mqtt5_outbound_topic_alias_behavior_type_to_c_string(
    enum aws_mqtt5_client_outbound_topic_alias_behavior_type outbound_aliasing_behavior) {
    switch (aws_mqtt5_outbound_topic_alias_behavior_type_to_non_default(outbound_aliasing_behavior)) {
        case AWS_MQTT5_COTABT_MANUAL:
            return "User-controlled outbound topic aliasing behavior";
        case AWS_MQTT5_COTABT_LRU:
            return "LRU caching outbound topic aliasing behavior";
        case AWS_MQTT5_COTABT_DISABLED:
            return "Outbound topic aliasing disabled";

        default:
            return "Unknown outbound topic aliasing behavior";
    }
}

bool aws_mqtt5_outbound_topic_alias_behavior_type_validate(
    enum aws_mqtt5_client_outbound_topic_alias_behavior_type outbound_aliasing_behavior) {

    return outbound_aliasing_behavior >= AWS_MQTT5_COTABT_DEFAULT &&
           outbound_aliasing_behavior <= AWS_MQTT5_COTABT_DISABLED;
}

enum aws_mqtt5_client_outbound_topic_alias_behavior_type aws_mqtt5_outbound_topic_alias_behavior_type_to_non_default(
    enum aws_mqtt5_client_outbound_topic_alias_behavior_type outbound_aliasing_behavior) {
    if (outbound_aliasing_behavior == AWS_MQTT5_COTABT_DEFAULT) {
        return AWS_MQTT5_COTABT_DISABLED;
    }

    return outbound_aliasing_behavior;
}

const char *aws_mqtt5_inbound_topic_alias_behavior_type_to_c_string(
    enum aws_mqtt5_client_inbound_topic_alias_behavior_type inbound_aliasing_behavior) {
    switch (aws_mqtt5_inbound_topic_alias_behavior_type_to_non_default(inbound_aliasing_behavior)) {
        case AWS_MQTT5_CITABT_ENABLED:
            return "Inbound topic aliasing behavior enabled";
        case AWS_MQTT5_CITABT_DISABLED:
            return "Inbound topic aliasing behavior disabled";
        default:
            return "Unknown inbound topic aliasing behavior";
    }
}

bool aws_mqtt5_inbound_topic_alias_behavior_type_validate(
    enum aws_mqtt5_client_inbound_topic_alias_behavior_type inbound_aliasing_behavior) {

    return inbound_aliasing_behavior >= AWS_MQTT5_CITABT_DEFAULT &&
           inbound_aliasing_behavior <= AWS_MQTT5_CITABT_DISABLED;
}

enum aws_mqtt5_client_inbound_topic_alias_behavior_type aws_mqtt5_inbound_topic_alias_behavior_type_to_non_default(
    enum aws_mqtt5_client_inbound_topic_alias_behavior_type inbound_aliasing_behavior) {
    if (inbound_aliasing_behavior == AWS_MQTT5_CITABT_DEFAULT) {
        return AWS_MQTT5_CITABT_DISABLED;
    }

    return inbound_aliasing_behavior;
}

const char *aws_mqtt5_extended_validation_and_flow_control_options_to_c_string(
    enum aws_mqtt5_extended_validation_and_flow_control_options extended_validation_behavior) {
    switch (extended_validation_behavior) {
        case AWS_MQTT5_EVAFCO_NONE:
            return "No additional flow control or packet validation";
        case AWS_MQTT5_EVAFCO_AWS_IOT_CORE_DEFAULTS:
            return "AWS IoT Core flow control and packet validation";
        default:
            return "Unknown extended validation behavior";
    }
}

const char *aws_mqtt5_client_operation_queue_behavior_type_to_c_string(
    enum aws_mqtt5_client_operation_queue_behavior_type offline_queue_behavior) {
    switch (aws_mqtt5_client_operation_queue_behavior_type_to_non_default(offline_queue_behavior)) {
        case AWS_MQTT5_COQBT_FAIL_NON_QOS1_PUBLISH_ON_DISCONNECT:
            return "Fail all incomplete operations except QoS 1 publishes";
        case AWS_MQTT5_COQBT_FAIL_QOS0_PUBLISH_ON_DISCONNECT:
            return "Fail incomplete QoS 0 publishes";
        case AWS_MQTT5_COQBT_FAIL_ALL_ON_DISCONNECT:
            return "Fail all incomplete operations";
        default:
            return "Unknown operation queue behavior type";
    }
}

enum aws_mqtt5_client_operation_queue_behavior_type aws_mqtt5_client_operation_queue_behavior_type_to_non_default(
    enum aws_mqtt5_client_operation_queue_behavior_type offline_queue_behavior) {
    if (offline_queue_behavior == AWS_MQTT5_COQBT_DEFAULT) {
        return AWS_MQTT5_COQBT_FAIL_QOS0_PUBLISH_ON_DISCONNECT;
    }

    return offline_queue_behavior;
}

const char *aws_mqtt5_client_lifecycle_event_type_to_c_string(
    enum aws_mqtt5_client_lifecycle_event_type lifecycle_event) {
    switch (lifecycle_event) {
        case AWS_MQTT5_CLET_ATTEMPTING_CONNECT:
            return "Connection establishment attempt";
        case AWS_MQTT5_CLET_CONNECTION_SUCCESS:
            return "Connection establishment success";
        case AWS_MQTT5_CLET_CONNECTION_FAILURE:
            return "Connection establishment failure";
        case AWS_MQTT5_CLET_DISCONNECTION:
            return "Disconnection";
        case AWS_MQTT5_CLET_STOPPED:
            return "Client stopped";
    }

    return "Unknown lifecycle event";
}

uint64_t aws_mqtt5_client_random_in_range(uint64_t from, uint64_t to) {
    uint64_t max = aws_max_u64(from, to);
    uint64_t min = aws_min_u64(from, to);

    /* Note: this contains several changes to the corresponding function in aws-c-io.  Don't throw them away.
     *
     * 1. random range is now inclusive/closed: [from, to] rather than half-open [from, to)
     * 2. as a corollary, diff == 0 => return min, not 0
     */
    uint64_t diff = max - min;
    if (!diff) {
        return min;
    }

    uint64_t random_value = 0;
    if (aws_device_random_u64(&random_value)) {
        return min;
    }

    if (diff == UINT64_MAX) {
        return random_value;
    }

    return min + random_value % (diff + 1); /* + 1 is safe due to previous check */
}

static uint8_t s_aws_iot_core_rules_prefix[] = "$aws/rules/";

static struct aws_byte_cursor s_aws_mqtt5_topic_skip_aws_iot_rules_prefix(struct aws_byte_cursor topic_cursor) {
    size_t prefix_length = AWS_ARRAY_SIZE(s_aws_iot_core_rules_prefix) - 1; /* skip 0-terminator */

    struct aws_byte_cursor rules_prefix = {
        .ptr = s_aws_iot_core_rules_prefix,
        .len = prefix_length,
    };

    if (topic_cursor.len < rules_prefix.len) {
        return topic_cursor;
    }

    struct aws_byte_cursor topic_cursor_copy = topic_cursor;
    struct aws_byte_cursor topic_prefix = topic_cursor;
    topic_prefix.len = rules_prefix.len;

    if (!aws_byte_cursor_eq_ignore_case(&rules_prefix, &topic_prefix)) {
        return topic_cursor;
    }

    aws_byte_cursor_advance(&topic_cursor_copy, prefix_length);
    if (topic_cursor_copy.len == 0) {
        return topic_cursor;
    }

    struct aws_byte_cursor rule_name_segment_cursor;
    AWS_ZERO_STRUCT(rule_name_segment_cursor);

    if (!aws_byte_cursor_next_split(&topic_cursor_copy, '/', &rule_name_segment_cursor)) {
        return topic_cursor;
    }

    if (topic_cursor_copy.len < rule_name_segment_cursor.len + 1) {
        return topic_cursor;
    }

    aws_byte_cursor_advance(&topic_cursor_copy, rule_name_segment_cursor.len + 1);

    return topic_cursor_copy;
}

static uint8_t s_shared_subscription_prefix[] = "$share";

static bool s_is_not_hash_or_plus(uint8_t byte) {
    return byte != '+' && byte != '#';
}

static struct aws_byte_cursor s_aws_mqtt5_topic_skip_shared_prefix(struct aws_byte_cursor topic_cursor) {
    /* shared subscription filters must have an initial segment of "$share" */
    struct aws_byte_cursor first_segment_cursor;
    AWS_ZERO_STRUCT(first_segment_cursor);
    if (!aws_byte_cursor_next_split(&topic_cursor, '/', &first_segment_cursor)) {
        return topic_cursor;
    }

    struct aws_byte_cursor share_prefix_cursor = {
        .ptr = s_shared_subscription_prefix,
        .len = AWS_ARRAY_SIZE(s_shared_subscription_prefix) - 1, /* skip null terminator */
    };

    if (!aws_byte_cursor_eq_ignore_case(&share_prefix_cursor, &first_segment_cursor)) {
        return topic_cursor;
    }

    /*
     * The next segment must be non-empty and cannot include '#', '/', or '+'.  In this case we know it already
     * does not include '/'
     */
    struct aws_byte_cursor second_segment_cursor = first_segment_cursor;
    if (!aws_byte_cursor_next_split(&topic_cursor, '/', &second_segment_cursor)) {
        return topic_cursor;
    }

    if (second_segment_cursor.len == 0 ||
        !aws_byte_cursor_satisfies_pred(&second_segment_cursor, s_is_not_hash_or_plus)) {
        return topic_cursor;
    }

    /*
     * Everything afterwards must form a normal, valid topic filter.
     */
    struct aws_byte_cursor remaining_cursor = topic_cursor;
    size_t remaining_length =
        topic_cursor.ptr + topic_cursor.len - (second_segment_cursor.len + second_segment_cursor.ptr);
    if (remaining_length == 0) {
        return topic_cursor;
    }

    aws_byte_cursor_advance(&remaining_cursor, topic_cursor.len - remaining_length + 1);

    return remaining_cursor;
}

struct aws_byte_cursor aws_mqtt5_topic_skip_aws_iot_core_uncounted_prefix(struct aws_byte_cursor topic_cursor) {
    struct aws_byte_cursor skip_shared = s_aws_mqtt5_topic_skip_shared_prefix(topic_cursor);
    struct aws_byte_cursor skip_rules = s_aws_mqtt5_topic_skip_aws_iot_rules_prefix(skip_shared);

    return skip_rules;
}

size_t aws_mqtt5_topic_get_segment_count(struct aws_byte_cursor topic_cursor) {
    size_t segment_count = 0;

    struct aws_byte_cursor segment_cursor;
    AWS_ZERO_STRUCT(segment_cursor);

    while (aws_byte_cursor_next_split(&topic_cursor, '/', &segment_cursor)) {
        ++segment_count;
    }

    return segment_count;
}

bool aws_mqtt_is_valid_topic_filter_for_iot_core(struct aws_byte_cursor topic_filter_cursor) {
    struct aws_byte_cursor post_rule_suffix = aws_mqtt5_topic_skip_aws_iot_core_uncounted_prefix(topic_filter_cursor);
    return aws_mqtt5_topic_get_segment_count(post_rule_suffix) <= AWS_IOT_CORE_MAXIMUM_TOPIC_SEGMENTS;
}

bool aws_mqtt_is_valid_topic_for_iot_core(struct aws_byte_cursor topic_cursor) {
    struct aws_byte_cursor post_rule_suffix = aws_mqtt5_topic_skip_aws_iot_core_uncounted_prefix(topic_cursor);
    if (aws_mqtt5_topic_get_segment_count(post_rule_suffix) > AWS_IOT_CORE_MAXIMUM_TOPIC_SEGMENTS) {
        return false;
    }

    return post_rule_suffix.len <= AWS_IOT_CORE_MAXIMUM_TOPIC_LENGTH;
}

/* $share/{ShareName}/{filter} */
bool aws_mqtt_is_topic_filter_shared_subscription(struct aws_byte_cursor topic_cursor) {
    struct aws_byte_cursor remaining_cursor = s_aws_mqtt5_topic_skip_shared_prefix(topic_cursor);
    if (remaining_cursor.len == topic_cursor.len) {
        return false;
    }

    if (!aws_mqtt_is_valid_topic_filter(&remaining_cursor)) {
        return false;
    }

    return true;
}
