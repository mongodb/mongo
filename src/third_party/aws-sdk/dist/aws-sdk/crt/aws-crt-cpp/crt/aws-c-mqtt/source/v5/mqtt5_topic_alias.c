/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/v5/mqtt5_topic_alias.h>

#include <aws/common/lru_cache.h>
#include <aws/common/string.h>
#include <aws/mqtt/private/client_impl_shared.h>
#include <aws/mqtt/private/v5/mqtt5_utils.h>

int aws_mqtt5_inbound_topic_alias_resolver_init(
    struct aws_mqtt5_inbound_topic_alias_resolver *resolver,
    struct aws_allocator *allocator) {
    AWS_ZERO_STRUCT(*resolver);
    resolver->allocator = allocator;

    if (aws_array_list_init_dynamic(&resolver->topic_aliases, allocator, 0, sizeof(struct aws_string *))) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static void s_release_aliases(struct aws_mqtt5_inbound_topic_alias_resolver *resolver) {
    if (!aws_array_list_is_valid(&resolver->topic_aliases)) {
        return;
    }

    size_t cache_size = aws_array_list_length(&resolver->topic_aliases);
    for (size_t i = 0; i < cache_size; ++i) {
        struct aws_string *topic = NULL;

        aws_array_list_get_at(&resolver->topic_aliases, &topic, i);
        aws_string_destroy(topic);
    }
}

void aws_mqtt5_inbound_topic_alias_resolver_clean_up(struct aws_mqtt5_inbound_topic_alias_resolver *resolver) {
    s_release_aliases(resolver);
    aws_array_list_clean_up(&resolver->topic_aliases);
}

int aws_mqtt5_inbound_topic_alias_resolver_reset(
    struct aws_mqtt5_inbound_topic_alias_resolver *resolver,
    uint16_t cache_size) {

    aws_mqtt5_inbound_topic_alias_resolver_clean_up(resolver);
    AWS_ZERO_STRUCT(resolver->topic_aliases);

    if (aws_array_list_init_dynamic(
            &resolver->topic_aliases, resolver->allocator, cache_size, sizeof(struct aws_string *))) {
        return AWS_OP_ERR;
    }

    for (size_t i = 0; i < cache_size; ++i) {
        struct aws_string *topic = NULL;
        aws_array_list_push_back(&resolver->topic_aliases, &topic);
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt5_inbound_topic_alias_resolver_resolve_alias(
    struct aws_mqtt5_inbound_topic_alias_resolver *resolver,
    uint16_t alias,
    struct aws_byte_cursor *topic_out) {
    size_t cache_size = aws_array_list_length(&resolver->topic_aliases);

    if (alias > cache_size || alias == 0) {
        return aws_raise_error(AWS_ERROR_MQTT5_INVALID_INBOUND_TOPIC_ALIAS);
    }

    size_t alias_index = alias - 1;
    struct aws_string *topic = NULL;
    aws_array_list_get_at(&resolver->topic_aliases, &topic, alias_index);

    if (topic == NULL) {
        return aws_raise_error(AWS_ERROR_MQTT5_INVALID_INBOUND_TOPIC_ALIAS);
    }

    *topic_out = aws_byte_cursor_from_string(topic);
    return AWS_OP_SUCCESS;
}

int aws_mqtt5_inbound_topic_alias_resolver_register_alias(
    struct aws_mqtt5_inbound_topic_alias_resolver *resolver,
    uint16_t alias,
    struct aws_byte_cursor topic) {
    size_t cache_size = aws_array_list_length(&resolver->topic_aliases);

    if (alias > cache_size || alias == 0) {
        return aws_raise_error(AWS_ERROR_MQTT5_INVALID_INBOUND_TOPIC_ALIAS);
    }

    struct aws_string *new_entry = aws_string_new_from_cursor(resolver->allocator, &topic);
    if (new_entry == NULL) {
        return AWS_OP_ERR;
    }

    size_t alias_index = alias - 1;
    struct aws_string *existing_entry = NULL;
    aws_array_list_get_at(&resolver->topic_aliases, &existing_entry, alias_index);
    aws_string_destroy(existing_entry);

    aws_array_list_set_at(&resolver->topic_aliases, &new_entry, alias_index);

    return AWS_OP_SUCCESS;
}

/****************************************************************************************************************/

struct aws_mqtt5_outbound_topic_alias_resolver_vtable {
    void (*destroy_fn)(struct aws_mqtt5_outbound_topic_alias_resolver *);
    int (*reset_fn)(struct aws_mqtt5_outbound_topic_alias_resolver *, uint16_t);
    int (*resolve_outbound_publish_fn)(
        struct aws_mqtt5_outbound_topic_alias_resolver *,
        const struct aws_mqtt5_packet_publish_view *,
        uint16_t *,
        struct aws_byte_cursor *);
};

struct aws_mqtt5_outbound_topic_alias_resolver {
    struct aws_allocator *allocator;

    struct aws_mqtt5_outbound_topic_alias_resolver_vtable *vtable;
    void *impl;
};

static struct aws_mqtt5_outbound_topic_alias_resolver *s_aws_mqtt5_outbound_topic_alias_resolver_disabled_new(
    struct aws_allocator *allocator);
static struct aws_mqtt5_outbound_topic_alias_resolver *s_aws_mqtt5_outbound_topic_alias_resolver_lru_new(
    struct aws_allocator *allocator);
static struct aws_mqtt5_outbound_topic_alias_resolver *s_aws_mqtt5_outbound_topic_alias_resolver_manual_new(
    struct aws_allocator *allocator);

struct aws_mqtt5_outbound_topic_alias_resolver *aws_mqtt5_outbound_topic_alias_resolver_new(
    struct aws_allocator *allocator,
    enum aws_mqtt5_client_outbound_topic_alias_behavior_type outbound_alias_behavior) {

    switch (aws_mqtt5_outbound_topic_alias_behavior_type_to_non_default(outbound_alias_behavior)) {
        case AWS_MQTT5_COTABT_MANUAL:
            return s_aws_mqtt5_outbound_topic_alias_resolver_manual_new(allocator);

        case AWS_MQTT5_COTABT_LRU:
            return s_aws_mqtt5_outbound_topic_alias_resolver_lru_new(allocator);

        case AWS_MQTT5_COTABT_DISABLED:
            return s_aws_mqtt5_outbound_topic_alias_resolver_disabled_new(allocator);

        default:
            return NULL;
    }
}

void aws_mqtt5_outbound_topic_alias_resolver_destroy(struct aws_mqtt5_outbound_topic_alias_resolver *resolver) {
    if (resolver == NULL) {
        return;
    }

    (*resolver->vtable->destroy_fn)(resolver);
}

int aws_mqtt5_outbound_topic_alias_resolver_reset(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver,
    uint16_t topic_alias_maximum) {

    if (resolver == NULL) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return (*resolver->vtable->reset_fn)(resolver, topic_alias_maximum);
}

int aws_mqtt5_outbound_topic_alias_resolver_resolve_outbound_publish(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver,
    const struct aws_mqtt5_packet_publish_view *publish_view,
    uint16_t *topic_alias_out,
    struct aws_byte_cursor *topic_out) {
    if (resolver == NULL) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return (*resolver->vtable->resolve_outbound_publish_fn)(resolver, publish_view, topic_alias_out, topic_out);
}

/*
 * Disabled resolver
 */

static void s_aws_mqtt5_outbound_topic_alias_resolver_disabled_destroy(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver) {
    if (resolver == NULL) {
        return;
    }

    aws_mem_release(resolver->allocator, resolver);
}

static int s_aws_mqtt5_outbound_topic_alias_resolver_disabled_reset(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver,
    uint16_t topic_alias_maximum) {
    (void)resolver;
    (void)topic_alias_maximum;

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_outbound_topic_alias_resolver_disabled_resolve_outbound_publish_fn(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver,
    const struct aws_mqtt5_packet_publish_view *publish_view,
    uint16_t *topic_alias_out,
    struct aws_byte_cursor *topic_out) {
    (void)resolver;

    if (publish_view->topic.len == 0) {
        return aws_raise_error(AWS_ERROR_MQTT5_PUBLISH_OPTIONS_VALIDATION);
    }

    *topic_alias_out = 0;
    *topic_out = publish_view->topic;

    return AWS_OP_SUCCESS;
}

static struct aws_mqtt5_outbound_topic_alias_resolver_vtable s_aws_mqtt5_outbound_topic_alias_resolver_disabled_vtable =
    {
        .destroy_fn = s_aws_mqtt5_outbound_topic_alias_resolver_disabled_destroy,
        .reset_fn = s_aws_mqtt5_outbound_topic_alias_resolver_disabled_reset,
        .resolve_outbound_publish_fn = s_aws_mqtt5_outbound_topic_alias_resolver_disabled_resolve_outbound_publish_fn,
};

static struct aws_mqtt5_outbound_topic_alias_resolver *s_aws_mqtt5_outbound_topic_alias_resolver_disabled_new(
    struct aws_allocator *allocator) {
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt5_outbound_topic_alias_resolver));

    resolver->allocator = allocator;
    resolver->vtable = &s_aws_mqtt5_outbound_topic_alias_resolver_disabled_vtable;

    return resolver;
}

/*
 * Manual resolver
 *
 * Manual resolution implies the user is controlling the topic alias assignments, but we still want to validate their
 * actions.  In particular, we track the currently valid set of aliases (based on previous outbound publishes)
 * and only use an alias when the submitted publish is an exact match for the current assignment.
 */

struct aws_mqtt5_outbound_topic_alias_resolver_manual {
    struct aws_mqtt5_outbound_topic_alias_resolver base;

    struct aws_array_list aliases;
};

static void s_cleanup_manual_aliases(struct aws_mqtt5_outbound_topic_alias_resolver_manual *manual_resolver) {
    for (size_t i = 0; i < aws_array_list_length(&manual_resolver->aliases); ++i) {
        struct aws_string *alias = NULL;
        aws_array_list_get_at(&manual_resolver->aliases, &alias, i);

        aws_string_destroy(alias);
    }

    aws_array_list_clean_up(&manual_resolver->aliases);
    AWS_ZERO_STRUCT(manual_resolver->aliases);
}

static void s_aws_mqtt5_outbound_topic_alias_resolver_manual_destroy(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver) {
    if (resolver == NULL) {
        return;
    }

    struct aws_mqtt5_outbound_topic_alias_resolver_manual *manual_resolver = resolver->impl;
    s_cleanup_manual_aliases(manual_resolver);

    aws_mem_release(resolver->allocator, manual_resolver);
}

static int s_aws_mqtt5_outbound_topic_alias_resolver_manual_reset(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver,
    uint16_t topic_alias_maximum) {
    struct aws_mqtt5_outbound_topic_alias_resolver_manual *manual_resolver = resolver->impl;
    s_cleanup_manual_aliases(manual_resolver);

    aws_array_list_init_dynamic(
        &manual_resolver->aliases, resolver->allocator, topic_alias_maximum, sizeof(struct aws_string *));
    for (size_t i = 0; i < topic_alias_maximum; ++i) {
        struct aws_string *invalid_alias = NULL;
        aws_array_list_push_back(&manual_resolver->aliases, &invalid_alias);
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_outbound_topic_alias_resolver_manual_resolve_outbound_publish_fn(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver,
    const struct aws_mqtt5_packet_publish_view *publish_view,
    uint16_t *topic_alias_out,
    struct aws_byte_cursor *topic_out) {

    if (publish_view->topic_alias == NULL) {
        /* not using a topic alias, nothing to do */
        *topic_alias_out = 0;
        *topic_out = publish_view->topic;

        return AWS_OP_SUCCESS;
    }

    uint16_t user_alias = *publish_view->topic_alias;
    if (user_alias == 0) {
        /* should have been caught by publish validation */
        return aws_raise_error(AWS_ERROR_MQTT5_INVALID_OUTBOUND_TOPIC_ALIAS);
    }

    struct aws_mqtt5_outbound_topic_alias_resolver_manual *manual_resolver = resolver->impl;
    uint16_t user_alias_index = user_alias - 1;
    if (user_alias_index >= aws_array_list_length(&manual_resolver->aliases)) {
        /* should have been caught by dynamic publish validation */
        return aws_raise_error(AWS_ERROR_MQTT5_INVALID_OUTBOUND_TOPIC_ALIAS);
    }

    struct aws_string *current_assignment = NULL;
    aws_array_list_get_at(&manual_resolver->aliases, &current_assignment, user_alias_index);

    *topic_alias_out = user_alias;

    bool can_use_alias = false;
    if (current_assignment != NULL) {
        struct aws_byte_cursor assignment_cursor = aws_byte_cursor_from_string(current_assignment);
        if (aws_byte_cursor_eq(&assignment_cursor, &publish_view->topic)) {
            can_use_alias = true;
        }
    }

    if (can_use_alias) {
        AWS_ZERO_STRUCT(*topic_out);
    } else {
        *topic_out = publish_view->topic;
    }

    /* mark this alias as seen */
    if (!can_use_alias) {
        aws_string_destroy(current_assignment);
        current_assignment = aws_string_new_from_cursor(resolver->allocator, &publish_view->topic);
        aws_array_list_set_at(&manual_resolver->aliases, &current_assignment, user_alias_index);
    }

    return AWS_OP_SUCCESS;
}

static struct aws_mqtt5_outbound_topic_alias_resolver_vtable s_aws_mqtt5_outbound_topic_alias_resolver_manual_vtable = {
    .destroy_fn = s_aws_mqtt5_outbound_topic_alias_resolver_manual_destroy,
    .reset_fn = s_aws_mqtt5_outbound_topic_alias_resolver_manual_reset,
    .resolve_outbound_publish_fn = s_aws_mqtt5_outbound_topic_alias_resolver_manual_resolve_outbound_publish_fn,
};

static struct aws_mqtt5_outbound_topic_alias_resolver *s_aws_mqtt5_outbound_topic_alias_resolver_manual_new(
    struct aws_allocator *allocator) {
    struct aws_mqtt5_outbound_topic_alias_resolver_manual *resolver =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt5_outbound_topic_alias_resolver_manual));

    resolver->base.allocator = allocator;
    resolver->base.vtable = &s_aws_mqtt5_outbound_topic_alias_resolver_manual_vtable;
    resolver->base.impl = resolver;

    aws_array_list_init_dynamic(&resolver->aliases, allocator, 0, sizeof(struct aws_string *));

    return &resolver->base;
}

/*
 * LRU resolver
 *
 * This resolver uses an LRU cache to automatically create topic alias assignments for the user.  With a reasonable
 * cache size, this should perform well for the majority of MQTT workloads.  For workloads it does not perform well
 * with, the user should control the assignment (or disable entirely).  Even for workloads where the LRU cache fails
 * to reuse an assignment every single time, the overall cost is 3 extra bytes per publish.  As a rough estimate, this
 * means that LRU topic aliasing is "worth it" if an existing alias can be used at least once every
 * (AverageTopicLength / 3) publishes.
 */

struct aws_mqtt5_outbound_topic_alias_resolver_lru {
    struct aws_mqtt5_outbound_topic_alias_resolver base;

    struct aws_cache *lru_cache;
    size_t max_aliases;
};

static void s_aws_mqtt5_outbound_topic_alias_resolver_lru_destroy(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver) {
    if (resolver == NULL) {
        return;
    }

    struct aws_mqtt5_outbound_topic_alias_resolver_lru *lru_resolver = resolver->impl;

    if (lru_resolver->lru_cache != NULL) {
        aws_cache_destroy(lru_resolver->lru_cache);
    }

    aws_mem_release(resolver->allocator, lru_resolver);
}

struct aws_topic_alias_assignment {
    struct aws_byte_cursor topic_cursor;
    struct aws_byte_buf topic;
    uint16_t alias;
    struct aws_allocator *allocator;
};

static void s_aws_topic_alias_assignment_destroy(struct aws_topic_alias_assignment *alias_assignment) {
    if (alias_assignment == NULL) {
        return;
    }

    aws_byte_buf_clean_up(&alias_assignment->topic);

    aws_mem_release(alias_assignment->allocator, alias_assignment);
}

static struct aws_topic_alias_assignment *s_aws_topic_alias_assignment_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor topic,
    uint16_t alias) {
    struct aws_topic_alias_assignment *assignment =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_topic_alias_assignment));

    assignment->allocator = allocator;
    assignment->alias = alias;

    if (aws_byte_buf_init_copy_from_cursor(&assignment->topic, allocator, topic)) {
        goto on_error;
    }

    assignment->topic_cursor = aws_byte_cursor_from_buf(&assignment->topic);

    return assignment;

on_error:

    s_aws_topic_alias_assignment_destroy(assignment);

    return NULL;
}

static void s_destroy_assignment_value(void *value) {
    s_aws_topic_alias_assignment_destroy(value);
}

static int s_aws_mqtt5_outbound_topic_alias_resolver_lru_reset(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver,
    uint16_t topic_alias_maximum) {
    struct aws_mqtt5_outbound_topic_alias_resolver_lru *lru_resolver = resolver->impl;

    if (lru_resolver->lru_cache != NULL) {
        aws_cache_destroy(lru_resolver->lru_cache);
        lru_resolver->lru_cache = NULL;
    }

    if (topic_alias_maximum > 0) {
        lru_resolver->lru_cache = aws_cache_new_lru(
            lru_resolver->base.allocator,
            aws_hash_byte_cursor_ptr,
            aws_mqtt_byte_cursor_hash_equality,
            NULL,
            s_destroy_assignment_value,
            topic_alias_maximum);
    }

    lru_resolver->max_aliases = topic_alias_maximum;

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_outbound_topic_alias_resolver_lru_resolve_outbound_publish_fn(
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver,
    const struct aws_mqtt5_packet_publish_view *publish_view,
    uint16_t *topic_alias_out,
    struct aws_byte_cursor *topic_out) {

    /* No cache => no aliasing done */
    struct aws_mqtt5_outbound_topic_alias_resolver_lru *lru_resolver = resolver->impl;
    if (lru_resolver->lru_cache == NULL || lru_resolver->max_aliases == 0) {
        *topic_alias_out = 0;
        *topic_out = publish_view->topic;
        return AWS_OP_SUCCESS;
    }

    /* Look for the topic in the cache */
    struct aws_byte_cursor topic = publish_view->topic;
    void *existing_element = NULL;
    if (aws_cache_find(lru_resolver->lru_cache, &topic, &existing_element)) {
        return AWS_OP_ERR;
    }

    struct aws_topic_alias_assignment *existing_assignment = existing_element;
    if (existing_assignment != NULL) {
        /*
         * Topic exists, so use the assignment. The LRU cache find implementation has already promoted the element
         * to MRU.
         */
        *topic_alias_out = existing_assignment->alias;
        AWS_ZERO_STRUCT(*topic_out);

        return AWS_OP_SUCCESS;
    }

    /* Topic doesn't exist in the cache. */
    uint16_t new_alias_id = 0;
    size_t assignment_count = aws_cache_get_element_count(lru_resolver->lru_cache);
    if (assignment_count == lru_resolver->max_aliases) {
        /*
         * The cache is full.  Get the LRU element to figure out what id we're going to reuse.  There's no way to get
         * the LRU element without promoting it.  So we get the element, save the discovered alias id, then remove
         * the element.
         */
        void *lru_element = aws_lru_cache_use_lru_element(lru_resolver->lru_cache);

        struct aws_topic_alias_assignment *replaced_assignment = lru_element;
        new_alias_id = replaced_assignment->alias;
        struct aws_byte_cursor replaced_topic = replaced_assignment->topic_cursor;

        /*
         * This is a little uncomfortable but valid.  The cursor we're passing in will get invalidated (and the backing
         * memory deleted) as part of the removal process but it is only used to find the element to remove.  Once
         * destruction begins it is no longer accessed.
         */
        aws_cache_remove(lru_resolver->lru_cache, &replaced_topic);
    } else {
        /*
         * The cache never shrinks and the first N adds are the N valid topic aliases.  Since the cache isn't full,
         * we know the next alias that hasn't been used.  This invariant only holds given that we will tear down
         * the connection (invalidating the cache) on errors from this function (ie, continuing on from a put
         * error would break the invariant and create duplicated ids).
         */
        new_alias_id = (uint16_t)(assignment_count + 1);
    }

    /*
     * We have a topic alias to use.  Add our new assignment.
     */
    struct aws_topic_alias_assignment *new_assignment =
        s_aws_topic_alias_assignment_new(resolver->allocator, topic, new_alias_id);
    if (new_assignment == NULL) {
        return AWS_OP_ERR;
    }

    /* the LRU cache put implementation automatically makes the newly added element MRU */
    if (aws_cache_put(lru_resolver->lru_cache, &new_assignment->topic_cursor, new_assignment)) {
        s_aws_topic_alias_assignment_destroy(new_assignment);
        return AWS_OP_ERR;
    }

    *topic_alias_out = new_assignment->alias;
    *topic_out = topic; /* this is a new assignment so topic must go out too */

    return AWS_OP_SUCCESS;
}

static struct aws_mqtt5_outbound_topic_alias_resolver_vtable s_aws_mqtt5_outbound_topic_alias_resolver_lru_vtable = {
    .destroy_fn = s_aws_mqtt5_outbound_topic_alias_resolver_lru_destroy,
    .reset_fn = s_aws_mqtt5_outbound_topic_alias_resolver_lru_reset,
    .resolve_outbound_publish_fn = s_aws_mqtt5_outbound_topic_alias_resolver_lru_resolve_outbound_publish_fn,
};

static struct aws_mqtt5_outbound_topic_alias_resolver *s_aws_mqtt5_outbound_topic_alias_resolver_lru_new(
    struct aws_allocator *allocator) {
    struct aws_mqtt5_outbound_topic_alias_resolver_lru *resolver =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt5_outbound_topic_alias_resolver_lru));

    resolver->base.allocator = allocator;
    resolver->base.vtable = &s_aws_mqtt5_outbound_topic_alias_resolver_lru_vtable;
    resolver->base.impl = resolver;

    return &resolver->base;
}
