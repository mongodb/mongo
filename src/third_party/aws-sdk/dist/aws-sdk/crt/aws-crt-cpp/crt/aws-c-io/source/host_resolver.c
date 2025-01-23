/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>

#include <aws/common/atomics.h>
#include <aws/common/clock.h>
#include <aws/common/condition_variable.h>
#include <aws/common/hash_table.h>
#include <aws/common/lru_cache.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/common/task_scheduler.h>
#include <aws/common/thread.h>

#include <aws/io/logging.h>

#include <inttypes.h>

const uint64_t NS_PER_SEC = 1000000000;
const size_t AWS_DEFAULT_DNS_TTL = 30;

int aws_host_address_copy(const struct aws_host_address *from, struct aws_host_address *to) {
    to->allocator = from->allocator;
    to->address = aws_string_new_from_string(to->allocator, from->address);
    to->host = aws_string_new_from_string(to->allocator, from->host);
    to->record_type = from->record_type;
    to->use_count = from->use_count;
    to->connection_failure_count = from->connection_failure_count;
    to->expiry = from->expiry;
    to->weight = from->weight;

    return AWS_OP_SUCCESS;
}

void aws_host_address_move(struct aws_host_address *from, struct aws_host_address *to) {
    to->allocator = from->allocator;
    to->address = from->address;
    to->host = from->host;
    to->record_type = from->record_type;
    to->use_count = from->use_count;
    to->connection_failure_count = from->connection_failure_count;
    to->expiry = from->expiry;
    to->weight = from->weight;
    AWS_ZERO_STRUCT(*from);
}

void aws_host_address_clean_up(struct aws_host_address *address) {
    if (address->address) {
        aws_string_destroy((void *)address->address);
    }
    if (address->host) {
        aws_string_destroy((void *)address->host);
    }
    AWS_ZERO_STRUCT(*address);
}

int aws_host_resolver_resolve_host(
    struct aws_host_resolver *resolver,
    const struct aws_string *host_name,
    aws_on_host_resolved_result_fn *res,
    const struct aws_host_resolution_config *config,
    void *user_data) {
    AWS_ASSERT(resolver->vtable && resolver->vtable->resolve_host);
    return resolver->vtable->resolve_host(resolver, host_name, res, config, user_data);
}

int aws_host_resolver_purge_cache(struct aws_host_resolver *resolver) {
    AWS_ASSERT(resolver->vtable && resolver->vtable->purge_cache);
    return resolver->vtable->purge_cache(resolver);
}

int aws_host_resolver_purge_cache_with_callback(
    struct aws_host_resolver *resolver,
    aws_simple_completion_callback *on_purge_cache_complete_callback,
    void *user_data) {
    AWS_PRECONDITION(resolver);
    AWS_PRECONDITION(resolver->vtable);

    if (!resolver->vtable->purge_cache_with_callback) {
        AWS_LOGF_ERROR(AWS_LS_IO_DNS, "purge_cache_with_callback function is not supported");
        return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
    }

    return resolver->vtable->purge_cache_with_callback(resolver, on_purge_cache_complete_callback, user_data);
}

int aws_host_resolver_purge_host_cache(
    struct aws_host_resolver *resolver,
    const struct aws_host_resolver_purge_host_options *options) {
    AWS_PRECONDITION(resolver);
    AWS_PRECONDITION(resolver->vtable);

    if (!resolver->vtable->purge_host_cache) {
        AWS_LOGF_ERROR(AWS_LS_IO_DNS, "purge_host_cache function is not supported");
        return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
    }
    return resolver->vtable->purge_host_cache(resolver, options);
}

int aws_host_resolver_record_connection_failure(
    struct aws_host_resolver *resolver,
    const struct aws_host_address *address) {
    AWS_ASSERT(resolver->vtable && resolver->vtable->record_connection_failure);
    return resolver->vtable->record_connection_failure(resolver, address);
}

/*
 * Used by both the resolver for its lifetime state as well as individual host entries for theirs.
 */
enum default_resolver_state {
    DRS_ACTIVE,
    DRS_SHUTTING_DOWN,
};

struct default_host_resolver {
    struct aws_allocator *allocator;

    /*
     * Mutually exclusion for the whole resolver, includes all member data and all host_entry_table operations.  Once
     * an entry is retrieved, this lock MAY be dropped but certain logic may hold both the resolver and the entry lock.
     * The two locks must be taken in that order.
     */
    struct aws_mutex resolver_lock;

    /* host_name (aws_string*) -> host_entry* */
    struct aws_hash_table host_entry_table;

    /* Hash table of listener entries per host name. We keep this decoupled from the host entry table to allow for
     * listeners to be added/removed regardless of whether or not a corresponding host entry exists.
     *
     * Any time the listener list in the listener entry becomes empty, we remove the entry from the table.  This
     * includes when a resolver thread moves all of the available listeners to its local list.
     */
    /* host_name (aws_string*) -> host_listener_entry* */
    struct aws_hash_table listener_entry_table;

    enum default_resolver_state state;

    /*
     * Tracks the number of launched resolution threads that have not yet invoked their shutdown completion
     * callback.
     */
    uint32_t pending_host_entry_shutdown_completion_callbacks;

    /*
     * Function to use to query current time.  Overridable in construction options.
     */
    aws_io_clock_fn *system_clock_fn;

    struct aws_event_loop_group *event_loop_group;
};

struct host_entry {
    /* immutable post-creation */
    struct aws_allocator *allocator;
    struct aws_host_resolver *resolver;
    struct aws_thread resolver_thread;
    const struct aws_string *host_name;
    int64_t resolve_frequency_ns;
    struct aws_host_resolution_config resolution_config;

    /* synchronized data and its lock */
    struct aws_mutex entry_lock;
    struct aws_condition_variable entry_signal;
    struct aws_cache *aaaa_records;
    struct aws_cache *a_records;
    struct aws_cache *failed_connection_aaaa_records;
    struct aws_cache *failed_connection_a_records;
    struct aws_linked_list pending_resolution_callbacks;
    uint32_t resolves_since_last_request;
    uint64_t last_resolve_request_timestamp_ns;
    enum default_resolver_state state;
    struct aws_array_list new_addresses;
    struct aws_array_list expired_addresses;
    aws_simple_completion_callback *on_host_purge_complete;
    void *on_host_purge_complete_user_data;
};

/*
 * A host entry's caches hold things of this type.  By using this and not the host_address directly, our
 * on_remove callbacks for the cache have access to the host_entry.  We wouldn't need to do this if those
 * callbacks supported user data injection, but they don't and too many internal code bases already depend
 * on the public API.
 */
struct aws_host_address_cache_entry {
    struct aws_host_address address;
    struct host_entry *entry;
};

int aws_host_address_cache_entry_copy(
    const struct aws_host_address_cache_entry *from,
    struct aws_host_address_cache_entry *to) {
    if (aws_host_address_copy(&from->address, &to->address)) {
        return AWS_OP_ERR;
    }

    to->entry = from->entry;

    return AWS_OP_SUCCESS;
}

static void s_shutdown_host_entry(struct host_entry *entry) {
    aws_mutex_lock(&entry->entry_lock);
    entry->state = DRS_SHUTTING_DOWN;
    /*
     * intentionally signal under the lock; we can't guarantee the resolver
     * is still around once the lock is released.
     */
    aws_condition_variable_notify_all(&entry->entry_signal);
    aws_mutex_unlock(&entry->entry_lock);
}

struct host_purge_callback_options {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;
    aws_simple_completion_callback *on_purge_cache_complete_callback;
    void *user_data;
};

static void s_host_purge_callback_options_destroy(void *user_data) {
    struct host_purge_callback_options *options = user_data;
    options->on_purge_cache_complete_callback(options->user_data);
    aws_mem_release(options->allocator, options);
}

static struct host_purge_callback_options *s_host_purge_callback_options_new(
    struct aws_allocator *allocator,
    aws_simple_completion_callback *on_purge_cache_complete_callback,
    void *user_data) {
    struct host_purge_callback_options *purge_callback_options =
        aws_mem_calloc(allocator, 1, sizeof(struct host_purge_callback_options));
    purge_callback_options->allocator = allocator;
    aws_ref_count_init(
        &purge_callback_options->ref_count, purge_callback_options, s_host_purge_callback_options_destroy);
    purge_callback_options->on_purge_cache_complete_callback = on_purge_cache_complete_callback;
    purge_callback_options->user_data = user_data;
    return purge_callback_options;
}

static void s_purge_cache_callback(void *user_data) {
    struct host_purge_callback_options *purge_callback_options = user_data;
    aws_ref_count_release(&purge_callback_options->ref_count);
}

/*
 * resolver lock must be held before calling this function
 */
static void s_clear_default_resolver_entry_table_synced(struct default_host_resolver *resolver) {
    struct aws_hash_table *table = &resolver->host_entry_table;
    for (struct aws_hash_iter iter = aws_hash_iter_begin(table); !aws_hash_iter_done(&iter);
         aws_hash_iter_next(&iter)) {
        struct host_entry *entry = iter.element.value;
        s_shutdown_host_entry(entry);
    }

    aws_hash_table_clear(table);
}

static int s_resolver_purge_cache(struct aws_host_resolver *resolver) {
    struct default_host_resolver *default_host_resolver = resolver->impl;
    aws_mutex_lock(&default_host_resolver->resolver_lock);
    s_clear_default_resolver_entry_table_synced(default_host_resolver);
    aws_mutex_unlock(&default_host_resolver->resolver_lock);

    return AWS_OP_SUCCESS;
}

static void s_purge_host_cache_callback_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)status;
    struct host_purge_callback_options *options = arg;
    aws_mem_release(options->allocator, task);
    aws_ref_count_release(&options->ref_count);
}

static void s_sechdule_purge_cache_callback_async(
    struct default_host_resolver *default_host_resolver,
    struct host_purge_callback_options *purge_callback_options) {

    struct aws_task *task = aws_mem_calloc(default_host_resolver->allocator, 1, sizeof(struct aws_task));
    aws_task_init(task, s_purge_host_cache_callback_task, purge_callback_options, "async_purge_host_callback_task");

    struct aws_event_loop *loop = aws_event_loop_group_get_next_loop(default_host_resolver->event_loop_group);
    AWS_FATAL_ASSERT(loop != NULL);
    aws_event_loop_schedule_task_now(loop, task);
}

static int s_resolver_purge_cache_with_callback(
    struct aws_host_resolver *resolver,
    aws_simple_completion_callback *on_purge_cache_complete_callback,
    void *user_data) {

    if (!on_purge_cache_complete_callback) {
        return s_resolver_purge_cache(resolver);
    }

    struct default_host_resolver *default_host_resolver = resolver->impl;
    aws_mutex_lock(&default_host_resolver->resolver_lock);
    struct aws_hash_table *table = &default_host_resolver->host_entry_table;

    struct host_purge_callback_options *purge_callback_options = s_host_purge_callback_options_new(
        default_host_resolver->allocator, on_purge_cache_complete_callback, user_data);

    /* purge all cache */
    for (struct aws_hash_iter iter = aws_hash_iter_begin(table); !aws_hash_iter_done(&iter);
         aws_hash_iter_next(&iter)) {
        struct host_entry *entry = iter.element.value;
        /* acquire a refernce to wait for the callback to trigger */
        aws_ref_count_acquire(&purge_callback_options->ref_count);
        aws_mutex_lock(&entry->entry_lock);
        entry->on_host_purge_complete = s_purge_cache_callback;
        entry->on_host_purge_complete_user_data = purge_callback_options;
        entry->state = DRS_SHUTTING_DOWN;
        aws_mutex_unlock(&entry->entry_lock);
    }

    aws_hash_table_clear(table);
    aws_mutex_unlock(&default_host_resolver->resolver_lock);

    /* release the original reference async */
    s_sechdule_purge_cache_callback_async(default_host_resolver, purge_callback_options);
    return AWS_OP_SUCCESS;
}

static void s_cleanup_default_resolver(struct aws_host_resolver *resolver) {
    struct default_host_resolver *default_host_resolver = resolver->impl;

    aws_event_loop_group_release(default_host_resolver->event_loop_group);
    aws_hash_table_clean_up(&default_host_resolver->host_entry_table);
    aws_hash_table_clean_up(&default_host_resolver->listener_entry_table);

    aws_mutex_clean_up(&default_host_resolver->resolver_lock);

    aws_simple_completion_callback *shutdown_callback = resolver->shutdown_options.shutdown_callback_fn;
    void *shutdown_completion_user_data = resolver->shutdown_options.shutdown_callback_user_data;

    aws_mem_release(resolver->allocator, resolver);

    /* invoke shutdown completion finally */
    if (shutdown_callback != NULL) {
        shutdown_callback(shutdown_completion_user_data);
    }
}

static void resolver_destroy(struct aws_host_resolver *resolver) {
    struct default_host_resolver *default_host_resolver = resolver->impl;

    bool cleanup_resolver = false;

    aws_mutex_lock(&default_host_resolver->resolver_lock);

    AWS_FATAL_ASSERT(default_host_resolver->state == DRS_ACTIVE);

    s_clear_default_resolver_entry_table_synced(default_host_resolver);
    default_host_resolver->state = DRS_SHUTTING_DOWN;
    if (default_host_resolver->pending_host_entry_shutdown_completion_callbacks == 0) {
        cleanup_resolver = true;
    }
    aws_mutex_unlock(&default_host_resolver->resolver_lock);

    if (cleanup_resolver) {
        s_cleanup_default_resolver(resolver);
    }
}

struct pending_callback {
    aws_on_host_resolved_result_fn *callback;
    void *user_data;
    struct aws_linked_list_node node;
};

static void s_clear_address_list(struct aws_array_list *address_list) {
    for (size_t i = 0; i < aws_array_list_length(address_list); ++i) {
        struct aws_host_address *address = NULL;
        aws_array_list_get_at_ptr(address_list, (void **)&address, i);
        aws_host_address_clean_up(address);
    }

    aws_array_list_clear(address_list);
}

static void s_clean_up_host_entry(struct host_entry *entry) {
    if (entry == NULL) {
        return;
    }

    /*
     * This can happen if the resolver's final reference drops while an unanswered query is pending on an entry.
     *
     * You could add an assertion that the resolver is in the shut down state if this condition hits but that
     * requires additional locking just to make the assert.
     */
    if (!aws_linked_list_empty(&entry->pending_resolution_callbacks)) {
        aws_raise_error(AWS_IO_DNS_HOST_REMOVED_FROM_CACHE);
    }

    while (!aws_linked_list_empty(&entry->pending_resolution_callbacks)) {
        struct aws_linked_list_node *resolution_callback_node =
            aws_linked_list_pop_front(&entry->pending_resolution_callbacks);
        struct pending_callback *pending_callback =
            AWS_CONTAINER_OF(resolution_callback_node, struct pending_callback, node);

        pending_callback->callback(
            entry->resolver, entry->host_name, AWS_IO_DNS_HOST_REMOVED_FROM_CACHE, NULL, pending_callback->user_data);

        aws_mem_release(entry->allocator, pending_callback);
    }

    aws_cache_destroy(entry->aaaa_records);
    aws_cache_destroy(entry->a_records);
    aws_cache_destroy(entry->failed_connection_a_records);
    aws_cache_destroy(entry->failed_connection_aaaa_records);
    aws_string_destroy((void *)entry->host_name);

    s_clear_address_list(&entry->new_addresses);
    aws_array_list_clean_up(&entry->new_addresses);

    s_clear_address_list(&entry->expired_addresses);
    aws_array_list_clean_up(&entry->expired_addresses);

    aws_mem_release(entry->allocator, entry);
}

static void s_on_host_entry_shutdown_completion(void *user_data) {
    struct host_entry *entry = user_data;
    struct aws_host_resolver *resolver = entry->resolver;
    struct default_host_resolver *default_host_resolver = resolver->impl;

    s_clean_up_host_entry(entry);

    bool cleanup_resolver = false;

    aws_mutex_lock(&default_host_resolver->resolver_lock);
    --default_host_resolver->pending_host_entry_shutdown_completion_callbacks;
    if (default_host_resolver->state == DRS_SHUTTING_DOWN &&
        default_host_resolver->pending_host_entry_shutdown_completion_callbacks == 0) {
        cleanup_resolver = true;
    }
    aws_mutex_unlock(&default_host_resolver->resolver_lock);

    if (cleanup_resolver) {
        s_cleanup_default_resolver(resolver);
    }
}

static int s_copy_address_into_array_list(struct aws_host_address *address, struct aws_array_list *address_list) {

    /*
     * This is the worst.
     *
     * We have to copy the cache address while we still have a write lock.  Otherwise, connection failures
     * can sneak in and destroy our address by moving the address to/from the various lru caches.
     *
     * But there's no nice copy construction into an array list, so we get to
     *   (1) Push a zeroed dummy element onto the array list
     *   (2) Get its pointer
     *   (3) Call aws_host_address_copy onto it.  If that fails, pop the dummy element.
     */
    struct aws_host_address dummy;
    AWS_ZERO_STRUCT(dummy);

    if (aws_array_list_push_back(address_list, &dummy)) {
        return AWS_OP_ERR;
    }

    struct aws_host_address *dest_copy = NULL;
    aws_array_list_get_at_ptr(address_list, (void **)&dest_copy, aws_array_list_length(address_list) - 1);
    AWS_FATAL_ASSERT(dest_copy != NULL);

    if (aws_host_address_copy(address, dest_copy)) {
        aws_array_list_pop_back(address_list);
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static uint64_t s_get_system_time_for_default_resolver(struct aws_host_resolver *resolver) {
    struct default_host_resolver *default_resolver = resolver->impl;

    uint64_t timestamp = 0;
    (*default_resolver->system_clock_fn)(&timestamp);

    return timestamp;
}

/* this only ever gets called after resolution has already run. We expect that the entry's lock
   has been acquired for writing before this function is called and released afterwards. */
static inline void process_records(
    struct host_entry *host_entry,
    struct aws_cache *records,
    struct aws_cache *failed_records) {

    struct aws_host_resolver *resolver = host_entry->resolver;
    uint64_t timestamp = s_get_system_time_for_default_resolver(resolver);

    size_t record_count = aws_cache_get_element_count(records);
    size_t expired_records = 0;

    /* since this only ever gets called after resolution has already run, we're in a dns outage
     * if everything is expired. Leave an element so we can keep trying. */
    for (size_t index = 0; index < record_count && expired_records < record_count - 1; ++index) {
        struct aws_host_address_cache_entry *lru_element_entry = aws_lru_cache_use_lru_element(records);

        if (lru_element_entry->address.expiry < timestamp) {
            AWS_LOGF_DEBUG(
                AWS_LS_IO_DNS,
                "static: purging expired record %s for %s",
                lru_element_entry->address.address->bytes,
                lru_element_entry->address.host->bytes);
            expired_records++;

            aws_cache_remove(records, lru_element_entry->address.address);
        }
    }

    record_count = aws_cache_get_element_count(records);
    AWS_LOGF_TRACE(AWS_LS_IO_DNS, "static: remaining record count for host %d", (int)record_count);

    /* if we don't have any known good addresses, take the least recently used, but not expired address with a history
     * of spotty behavior and upgrade it for reuse. If it's expired, leave it and let the resolve fail. Better to fail
     * than accidentally give a kids' app an IP address to somebody's adult website when the IP address gets rebound to
     * a different endpoint. The moral of the story here is to not disable SSL verification! */
    if (!record_count) {
        size_t failed_count = aws_cache_get_element_count(failed_records);
        for (size_t index = 0; index < failed_count; ++index) {
            struct aws_host_address_cache_entry *lru_element_entry = aws_lru_cache_use_lru_element(failed_records);
            if (timestamp >= lru_element_entry->address.expiry) {
                continue;
            }

            struct aws_host_address_cache_entry *to_add =
                aws_mem_calloc(host_entry->allocator, 1, sizeof(struct aws_host_address_cache_entry));
            if (to_add == NULL) {
                continue;
            }

            if (aws_host_address_cache_entry_copy(lru_element_entry, to_add) ||
                aws_cache_put(records, to_add->address.address, to_add)) {
                aws_host_address_clean_up(&to_add->address);
                aws_mem_release(host_entry->allocator, to_add);
                continue;
            }

            /*
             * Promoting an address from failed to good should trigger the new address callback
             */
            s_copy_address_into_array_list(&lru_element_entry->address, &host_entry->new_addresses);

            AWS_LOGF_INFO(
                AWS_LS_IO_DNS,
                "static: promoting spotty record %s for %s back to good list",
                lru_element_entry->address.address->bytes,
                lru_element_entry->address.host->bytes);

            aws_cache_remove(failed_records, lru_element_entry->address.address);

            /* we only want to promote one per process run.*/
            break;
        }
    }
}

static int s_resolver_purge_host_cache(
    struct aws_host_resolver *resolver,
    const struct aws_host_resolver_purge_host_options *options) {

    AWS_PRECONDITION(resolver);
    if (options == NULL) {
        AWS_LOGF_ERROR(AWS_LS_IO_DNS, "Cannot purge host cache; options structure is NULL.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct default_host_resolver *default_host_resolver = resolver->impl;
    AWS_LOGF_INFO(AWS_LS_IO_DNS, "id=%p: purging record for %s", (void *)resolver, options->host->bytes);

    aws_mutex_lock(&default_host_resolver->resolver_lock);

    struct aws_hash_element *element = NULL;
    aws_hash_table_find(&default_host_resolver->host_entry_table, options->host, &element);

    /* Success if entry doesn't exist in cache. */
    if (element == NULL) {
        aws_mutex_unlock(&default_host_resolver->resolver_lock);
        if (options->on_host_purge_complete_callback != NULL) {
            /* Schedule completion callback asynchronouly */
            struct host_purge_callback_options *purge_callback_options = s_host_purge_callback_options_new(
                default_host_resolver->allocator, options->on_host_purge_complete_callback, options->user_data);

            s_sechdule_purge_cache_callback_async(default_host_resolver, purge_callback_options);
        }
        return AWS_OP_SUCCESS;
    }

    struct host_entry *host_entry = element->value;
    AWS_FATAL_ASSERT(host_entry);

    /* Setup the on_host_purge_complete callback. */
    aws_mutex_lock(&host_entry->entry_lock);
    AWS_FATAL_ASSERT(!host_entry->on_host_purge_complete);
    AWS_FATAL_ASSERT(!host_entry->on_host_purge_complete_user_data);
    host_entry->on_host_purge_complete = options->on_host_purge_complete_callback;
    host_entry->on_host_purge_complete_user_data = options->user_data;
    aws_mutex_unlock(&host_entry->entry_lock);

    s_shutdown_host_entry(host_entry);
    aws_hash_table_remove_element(&default_host_resolver->host_entry_table, element);

    aws_mutex_unlock(&default_host_resolver->resolver_lock);
    return AWS_OP_SUCCESS;
}

static int resolver_record_connection_failure(
    struct aws_host_resolver *resolver,
    const struct aws_host_address *address) {
    struct default_host_resolver *default_host_resolver = resolver->impl;

    AWS_LOGF_INFO(
        AWS_LS_IO_DNS,
        "id=%p: recording failure for record %s for %s, moving to bad list",
        (void *)resolver,
        address->address->bytes,
        address->host->bytes);

    aws_mutex_lock(&default_host_resolver->resolver_lock);

    struct aws_hash_element *element = NULL;
    if (aws_hash_table_find(&default_host_resolver->host_entry_table, address->host, &element)) {
        aws_mutex_unlock(&default_host_resolver->resolver_lock);
        return AWS_OP_ERR;
    }

    struct host_entry *host_entry = NULL;
    if (element != NULL) {
        host_entry = element->value;
        AWS_FATAL_ASSERT(host_entry);
    }

    if (host_entry) {
        struct aws_host_address_cache_entry *cached_address_entry = NULL;

        aws_mutex_lock(&host_entry->entry_lock);
        aws_mutex_unlock(&default_host_resolver->resolver_lock);
        struct aws_cache *address_table =
            address->record_type == AWS_ADDRESS_RECORD_TYPE_AAAA ? host_entry->aaaa_records : host_entry->a_records;

        struct aws_cache *failed_table = address->record_type == AWS_ADDRESS_RECORD_TYPE_AAAA
                                             ? host_entry->failed_connection_aaaa_records
                                             : host_entry->failed_connection_a_records;

        aws_cache_find(address_table, address->address, (void **)&cached_address_entry);

        struct aws_host_address_cache_entry *address_entry_copy = NULL;
        if (cached_address_entry) {
            address_entry_copy = aws_mem_calloc(resolver->allocator, 1, sizeof(struct aws_host_address_cache_entry));

            if (!address_entry_copy || aws_host_address_cache_entry_copy(cached_address_entry, address_entry_copy)) {
                goto error_host_entry_cleanup;
            }

            /*
             * This will trigger an expiration callback since the good caches add the removed address to the
             * host_entry's expired list, via the cache's on_delete callback
             */
            if (aws_cache_remove(address_table, cached_address_entry->address.address)) {
                goto error_host_entry_cleanup;
            }

            address_entry_copy->address.connection_failure_count += 1;

            if (aws_cache_put(failed_table, address_entry_copy->address.address, address_entry_copy)) {
                goto error_host_entry_cleanup;
            }
        } else {
            if (aws_cache_find(failed_table, address->address, (void **)&cached_address_entry)) {
                goto error_host_entry_cleanup;
            }

            if (cached_address_entry) {
                cached_address_entry->address.connection_failure_count += 1;
            }
        }
        aws_mutex_unlock(&host_entry->entry_lock);
        return AWS_OP_SUCCESS;

    error_host_entry_cleanup:
        if (address_entry_copy) {
            aws_host_address_clean_up(&address_entry_copy->address);
            aws_mem_release(resolver->allocator, address_entry_copy);
        }
        aws_mutex_unlock(&host_entry->entry_lock);
        return AWS_OP_ERR;
    }

    aws_mutex_unlock(&default_host_resolver->resolver_lock);

    return AWS_OP_SUCCESS;
}

/*
 * A bunch of convenience functions for the host resolver background thread function
 */

static struct aws_host_address_cache_entry *s_find_cached_address_entry_aux(
    struct aws_cache *primary_records,
    struct aws_cache *fallback_records,
    const struct aws_string *address) {

    struct aws_host_address_cache_entry *found = NULL;
    aws_cache_find(primary_records, address, (void **)&found);
    if (found == NULL) {
        aws_cache_find(fallback_records, address, (void **)&found);
    }

    return found;
}

/*
 * Looks in both the good and failed connection record sets for a given host record
 */
static struct aws_host_address_cache_entry *s_find_cached_address_entry(
    struct host_entry *entry,
    const struct aws_string *address,
    enum aws_address_record_type record_type) {

    switch (record_type) {
        case AWS_ADDRESS_RECORD_TYPE_AAAA:
            return s_find_cached_address_entry_aux(entry->aaaa_records, entry->failed_connection_aaaa_records, address);

        case AWS_ADDRESS_RECORD_TYPE_A:
            return s_find_cached_address_entry_aux(entry->a_records, entry->failed_connection_a_records, address);

        default:
            return NULL;
    }
}

static struct aws_host_address_cache_entry *s_get_lru_address_entry_aux(
    struct aws_cache *primary_records,
    struct aws_cache *fallback_records) {

    struct aws_host_address_cache_entry *address_entry = aws_lru_cache_use_lru_element(primary_records);
    if (address_entry == NULL) {
        aws_lru_cache_use_lru_element(fallback_records);
    }

    return address_entry;
}

/*
 * Looks in both the good and failed connection record sets for the LRU host record
 */
static struct aws_host_address_cache_entry *s_get_lru_address(
    struct host_entry *entry,
    enum aws_address_record_type record_type) {
    switch (record_type) {
        case AWS_ADDRESS_RECORD_TYPE_AAAA:
            return s_get_lru_address_entry_aux(entry->aaaa_records, entry->failed_connection_aaaa_records);

        case AWS_ADDRESS_RECORD_TYPE_A:
            return s_get_lru_address_entry_aux(entry->a_records, entry->failed_connection_a_records);

        default:
            return NULL;
    }
}

static void s_update_address_cache(
    struct host_entry *host_entry,
    struct aws_array_list *address_list,
    uint64_t new_expiration) {

    AWS_PRECONDITION(host_entry);
    AWS_PRECONDITION(address_list);

    for (size_t i = 0; i < aws_array_list_length(address_list); ++i) {
        struct aws_host_address *fresh_resolved_address = NULL;
        aws_array_list_get_at_ptr(address_list, (void **)&fresh_resolved_address, i);

        struct aws_host_address_cache_entry *address_to_cache_entry = s_find_cached_address_entry(
            host_entry, fresh_resolved_address->address, fresh_resolved_address->record_type);

        if (address_to_cache_entry) {
            address_to_cache_entry->address.expiry = new_expiration;
            AWS_LOGF_TRACE(
                AWS_LS_IO_DNS,
                "static: updating expiry for %s for host %s to %llu",
                address_to_cache_entry->address.address->bytes,
                host_entry->host_name->bytes,
                (unsigned long long)new_expiration);
        } else {
            address_to_cache_entry =
                aws_mem_calloc(host_entry->allocator, 1, sizeof(struct aws_host_address_cache_entry));

            aws_host_address_move(fresh_resolved_address, &address_to_cache_entry->address);
            address_to_cache_entry->address.expiry = new_expiration;
            address_to_cache_entry->entry = host_entry;

            struct aws_cache *address_table =
                address_to_cache_entry->address.record_type == AWS_ADDRESS_RECORD_TYPE_AAAA ? host_entry->aaaa_records
                                                                                            : host_entry->a_records;

            if (aws_cache_put(address_table, address_to_cache_entry->address.address, address_to_cache_entry)) {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_DNS,
                    "static: could not add new address to host entry cache for host '%s' in "
                    "s_update_address_cache.",
                    host_entry->host_name->bytes);

                continue;
            }

            AWS_LOGF_DEBUG(
                AWS_LS_IO_DNS,
                "static: new address resolved %s for host %s caching",
                address_to_cache_entry->address.address->bytes,
                host_entry->host_name->bytes);

            struct aws_host_address new_address_copy;

            if (aws_host_address_copy(&address_to_cache_entry->address, &new_address_copy)) {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_DNS,
                    "static: could not copy address for new-address list for host '%s' in s_update_address_cache.",
                    host_entry->host_name->bytes);

                continue;
            }

            if (aws_array_list_push_back(&host_entry->new_addresses, &new_address_copy)) {
                aws_host_address_clean_up(&new_address_copy);

                AWS_LOGF_ERROR(
                    AWS_LS_IO_DNS,
                    "static: could not push address to new-address list for host '%s' in s_update_address_cache.",
                    host_entry->host_name->bytes);

                continue;
            }
        }
    }
}

static void s_copy_address_into_callback_set(
    struct aws_host_address_cache_entry *entry,
    struct aws_array_list *callback_addresses,
    const struct aws_string *host_name) {

    if (entry != NULL) {
        if (s_copy_address_into_array_list(&entry->address, callback_addresses)) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_DNS,
                "static: failed to vend address %s for host %s to caller",
                entry->address.address->bytes,
                host_name->bytes);
            return;
        }

        entry->address.use_count += 1;

        AWS_LOGF_TRACE(
            AWS_LS_IO_DNS,
            "static: vending address %s for host %s to caller",
            entry->address.address->bytes,
            host_name->bytes);
    }
}

static bool s_host_entry_finished_pred(void *user_data) {
    struct host_entry *entry = user_data;

    return entry->state == DRS_SHUTTING_DOWN;
}

static bool s_host_entry_finished_or_pending_request_pred(void *user_data) {
    struct host_entry *entry = user_data;

    return entry->state == DRS_SHUTTING_DOWN || !aws_linked_list_empty(&entry->pending_resolution_callbacks);
}

static const uint64_t AWS_MINIMUM_WAIT_BETWEEN_DNS_QUERIES_NS = 100000000; /* 100 ms */

static void aws_host_resolver_thread(void *arg) {
    struct host_entry *host_entry = arg;

    uint64_t max_no_solicitation_interval = aws_timestamp_convert(
        aws_max_u64(1, host_entry->resolution_config.max_ttl), AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);

    uint64_t wait_between_resolves_interval =
        aws_min_u64(max_no_solicitation_interval, host_entry->resolve_frequency_ns);

    uint64_t shutdown_only_wait_time = AWS_MINIMUM_WAIT_BETWEEN_DNS_QUERIES_NS;
    uint64_t request_interruptible_wait_time = 0;
    if (wait_between_resolves_interval > shutdown_only_wait_time) {
        request_interruptible_wait_time = wait_between_resolves_interval - shutdown_only_wait_time;
    }

    struct aws_linked_list listener_list;
    aws_linked_list_init(&listener_list);

    struct aws_linked_list listener_destroy_list;
    aws_linked_list_init(&listener_destroy_list);

    bool keep_going = true;

    struct aws_array_list address_list;
    AWS_ZERO_STRUCT(address_list);
    struct aws_array_list new_address_list;
    AWS_ZERO_STRUCT(new_address_list);
    struct aws_array_list expired_address_list;
    AWS_ZERO_STRUCT(expired_address_list);

    if (aws_array_list_init_dynamic(&address_list, host_entry->allocator, 4, sizeof(struct aws_host_address))) {
        goto done;
    }

    if (aws_array_list_init_dynamic(&new_address_list, host_entry->allocator, 4, sizeof(struct aws_host_address))) {
        goto done;
    }

    if (aws_array_list_init_dynamic(&expired_address_list, host_entry->allocator, 4, sizeof(struct aws_host_address))) {
        goto done;
    }

    while (keep_going) {

        /* resolve and then process each record */
        int err_code = AWS_ERROR_SUCCESS;
        if (host_entry->resolution_config.impl(
                host_entry->allocator, host_entry->host_name, &address_list, host_entry->resolution_config.impl_data)) {

            err_code = aws_last_error();
        }

        if (err_code == AWS_ERROR_SUCCESS) {
            AWS_LOGF_DEBUG(
                AWS_LS_IO_DNS,
                "static, resolving host %s successful, returned %d addresses",
                aws_string_c_str(host_entry->host_name),
                (int)aws_array_list_length(&address_list));
        } else {
            AWS_LOGF_WARN(
                AWS_LS_IO_DNS,
                "static, resolving host %s failed, ec %d (%s)",
                aws_string_c_str(host_entry->host_name),
                err_code,
                aws_error_debug_str(err_code));
        }

        uint64_t timestamp = s_get_system_time_for_default_resolver(host_entry->resolver);
        uint64_t new_expiry = timestamp + (host_entry->resolution_config.max_ttl * NS_PER_SEC);

        struct aws_linked_list pending_resolve_copy;
        aws_linked_list_init(&pending_resolve_copy);

        /*
         * Within the lock we
         *  (1) Update the cache with the newly resolved addresses
         *  (2) Process all held addresses looking for expired or promotable ones
         *  (3) Prep for callback invocations
         */
        aws_mutex_lock(&host_entry->entry_lock);

        if (!err_code) {
            s_update_address_cache(host_entry, &address_list, new_expiry);
        }

        /*
         * process and clean_up records in the entry. occasionally, failed connect records will be upgraded
         * for retry.
         */
        process_records(host_entry, host_entry->aaaa_records, host_entry->failed_connection_aaaa_records);
        process_records(host_entry, host_entry->a_records, host_entry->failed_connection_a_records);

        aws_linked_list_swap_contents(&pending_resolve_copy, &host_entry->pending_resolution_callbacks);

        aws_mutex_unlock(&host_entry->entry_lock);

        /*
         * Clean up resolved addressed outside of the lock
         */
        s_clear_address_list(&address_list);

        struct aws_host_address address_array[2];
        AWS_ZERO_ARRAY(address_array);

        /*
         * Perform the actual subscriber notifications
         */
        while (!aws_linked_list_empty(&pending_resolve_copy)) {
            struct aws_linked_list_node *resolution_callback_node = aws_linked_list_pop_front(&pending_resolve_copy);
            struct pending_callback *pending_callback =
                AWS_CONTAINER_OF(resolution_callback_node, struct pending_callback, node);

            struct aws_array_list callback_address_list;
            aws_array_list_init_static(&callback_address_list, address_array, 2, sizeof(struct aws_host_address));

            aws_mutex_lock(&host_entry->entry_lock);
            s_copy_address_into_callback_set(
                s_get_lru_address(host_entry, AWS_ADDRESS_RECORD_TYPE_AAAA),
                &callback_address_list,
                host_entry->host_name);
            s_copy_address_into_callback_set(
                s_get_lru_address(host_entry, AWS_ADDRESS_RECORD_TYPE_A),
                &callback_address_list,
                host_entry->host_name);
            aws_mutex_unlock(&host_entry->entry_lock);

            size_t callback_address_list_size = aws_array_list_length(&callback_address_list);
            if (callback_address_list_size > 0) {
                AWS_LOGF_DEBUG(
                    AWS_LS_IO_DNS,
                    "static, invoking resolution callback for host %s with %d addresses",
                    aws_string_c_str(host_entry->host_name),
                    (int)callback_address_list_size);
            } else {
                AWS_LOGF_DEBUG(
                    AWS_LS_IO_DNS,
                    "static, invoking resolution callback for host %s with failure",
                    aws_string_c_str(host_entry->host_name));
            }

            if (callback_address_list_size > 0) {
                pending_callback->callback(
                    host_entry->resolver,
                    host_entry->host_name,
                    AWS_OP_SUCCESS,
                    &callback_address_list,
                    pending_callback->user_data);

            } else {
                int error_code = (err_code != AWS_ERROR_SUCCESS) ? err_code : AWS_IO_DNS_QUERY_FAILED;
                pending_callback->callback(
                    host_entry->resolver, host_entry->host_name, error_code, NULL, pending_callback->user_data);
            }

            s_clear_address_list(&callback_address_list);

            aws_mem_release(host_entry->allocator, pending_callback);
        }

        aws_mutex_lock(&host_entry->entry_lock);

        ++host_entry->resolves_since_last_request;

        /*
         * A long resolve frequency matched with a connection failure can induce a state of DNS starvation, where
         * additional resolution requests go into the queue but since there's no good records and the thread is sleeping
         * for a long time, nothing happens.
         *
         * While we could make the wait predicate also check the queue of requests, there is a worry that a
         * host that can't be resolved (user error, dns record removal, etc...) could lead to a "spammy" scenario
         * where the thread generates DNS requests extremely quickly, ie, the sleep becomes almost instant.
         *
         * We'd like to be able to express the wait here as something a bit more complex:
         *
         * "Wait until either (1) shutdown notice, or (2) a small amount of time has passed and there are pending
         * requests, or (3) the resolution interval has passed"
         *
         * While seemingly complicated, we can do this actually just by chaining two waits:
         *
         * (1) The first wait is for a short amount of time and only predicates on the shutdown notice
         * (2) The second wait is for the remaining frequency interval and predicates on either the shutdown notice
         *     or a pending resolve request
         *
         * This leaves us with wait behavior where:
         *  (1) Shutdown always fully interrupts and immediately causes the thread function to complete
         *  (2) Absent shutdown, there is always a controllable, non-trivial sleep between resolves
         *  (3) Starvation is avoided as pending requests can wake the resolver thread independent of resolution
         *      frequency
         */
        aws_condition_variable_wait_for_pred(
            &host_entry->entry_signal,
            &host_entry->entry_lock,
            shutdown_only_wait_time,
            s_host_entry_finished_pred,
            host_entry);

        if (request_interruptible_wait_time > 0) {
            aws_condition_variable_wait_for_pred(
                &host_entry->entry_signal,
                &host_entry->entry_lock,
                request_interruptible_wait_time,
                s_host_entry_finished_or_pending_request_pred,
                host_entry);
        }

        aws_mutex_unlock(&host_entry->entry_lock);

        /*
         * This is a bit awkward that we unlock the entry and then relock both the resolver and the entry, but it
         * is mandatory that -- in order to maintain the consistent view of the resolver table (entry exist => entry
         * is alive and can be queried) -- we have the resolver lock as well before making the decision to remove
         * the entry from the table and terminate the thread.
         */
        struct default_host_resolver *resolver = host_entry->resolver->impl;
        aws_mutex_lock(&resolver->resolver_lock);

        aws_mutex_lock(&host_entry->entry_lock);

        uint64_t now = s_get_system_time_for_default_resolver(host_entry->resolver);

        /*
         * The only way we terminate the loop with pending queries is if the resolver itself has no more references
         * to it and is going away.  In that case, the pending queries will be completed (with failure) by the
         * final clean up of this entry.
         */
        if (aws_linked_list_empty(&host_entry->pending_resolution_callbacks) &&
            host_entry->last_resolve_request_timestamp_ns + max_no_solicitation_interval < now) {
            host_entry->state = DRS_SHUTTING_DOWN;
        }

        keep_going = host_entry->state == DRS_ACTIVE;
        if (!keep_going) {
            aws_hash_table_remove(&resolver->host_entry_table, host_entry->host_name, NULL, NULL);
        }

        aws_array_list_swap_contents(&host_entry->new_addresses, &new_address_list);
        aws_array_list_swap_contents(&host_entry->expired_addresses, &expired_address_list);

        aws_mutex_unlock(&host_entry->entry_lock);
        aws_mutex_unlock(&resolver->resolver_lock);

        s_clear_address_list(&new_address_list);
        s_clear_address_list(&expired_address_list);
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_DNS,
        "static: Either no requests have been made for an address for %s for the duration "
        "of the ttl, or this thread is being forcibly shutdown. Killing thread.",
        host_entry->host_name->bytes);

done:

    AWS_FATAL_ASSERT(aws_array_list_length(&address_list) == 0);
    AWS_FATAL_ASSERT(aws_array_list_length(&new_address_list) == 0);
    AWS_FATAL_ASSERT(aws_array_list_length(&expired_address_list) == 0);

    aws_array_list_clean_up(&address_list);
    aws_array_list_clean_up(&new_address_list);
    aws_array_list_clean_up(&expired_address_list);

    /* trigger the purge complete callback */
    if (host_entry->on_host_purge_complete != NULL) {
        host_entry->on_host_purge_complete(host_entry->on_host_purge_complete_user_data);
    }
    /* please don't fail */
    aws_thread_current_at_exit(s_on_host_entry_shutdown_completion, host_entry);
}

static void on_cache_entry_removed_helper(struct aws_host_address_cache_entry *entry) {
    AWS_LOGF_DEBUG(
        AWS_LS_IO_DNS,
        "static: purging address %s for host %s from "
        "the cache due to cache eviction or shutdown",
        entry->address.address->bytes,
        entry->address.host->bytes);

    struct aws_allocator *allocator = entry->address.allocator;
    aws_host_address_clean_up(&entry->address);
    aws_mem_release(allocator, entry);
}

static void on_good_address_entry_removed(void *value) {
    struct aws_host_address_cache_entry *entry = value;
    if (entry == NULL) {
        return;
    }

    s_copy_address_into_array_list(&entry->address, &entry->entry->expired_addresses);

    on_cache_entry_removed_helper(entry);
}

static void on_failed_address_entry_removed(void *value) {
    struct aws_host_address_cache_entry *entry = value;

    on_cache_entry_removed_helper(entry);
}

/*
 * The resolver lock must be held before calling this function
 */
static inline int create_and_init_host_entry(
    struct aws_host_resolver *resolver,
    const struct aws_string *host_name,
    aws_on_host_resolved_result_fn *res,
    const struct aws_host_resolution_config *config,
    uint64_t timestamp,
    void *user_data) {
    struct host_entry *new_host_entry = aws_mem_calloc(resolver->allocator, 1, sizeof(struct host_entry));
    if (!new_host_entry) {
        return AWS_OP_ERR;
    }

    new_host_entry->resolver = resolver;
    new_host_entry->allocator = resolver->allocator;
    new_host_entry->last_resolve_request_timestamp_ns = timestamp;
    new_host_entry->resolves_since_last_request = 0;
    new_host_entry->resolve_frequency_ns =
        (config->resolve_frequency_ns != 0) ? config->resolve_frequency_ns : NS_PER_SEC;
    new_host_entry->state = DRS_ACTIVE;

    bool thread_init = false;
    struct pending_callback *pending_callback = NULL;
    const struct aws_string *host_string_copy = aws_string_new_from_string(resolver->allocator, host_name);
    if (AWS_UNLIKELY(!host_string_copy)) {
        goto setup_host_entry_error;
    }

    new_host_entry->host_name = host_string_copy;
    new_host_entry->a_records = aws_cache_new_lru(
        new_host_entry->allocator,
        aws_hash_string,
        aws_hash_callback_string_eq,
        NULL,
        on_good_address_entry_removed,
        config->max_ttl);
    if (AWS_UNLIKELY(!new_host_entry->a_records)) {
        goto setup_host_entry_error;
    }

    new_host_entry->aaaa_records = aws_cache_new_lru(
        new_host_entry->allocator,
        aws_hash_string,
        aws_hash_callback_string_eq,
        NULL,
        on_good_address_entry_removed,
        config->max_ttl);
    if (AWS_UNLIKELY(!new_host_entry->aaaa_records)) {
        goto setup_host_entry_error;
    }

    new_host_entry->failed_connection_a_records = aws_cache_new_lru(
        new_host_entry->allocator,
        aws_hash_string,
        aws_hash_callback_string_eq,
        NULL,
        on_failed_address_entry_removed,
        config->max_ttl);
    if (AWS_UNLIKELY(!new_host_entry->failed_connection_a_records)) {
        goto setup_host_entry_error;
    }

    new_host_entry->failed_connection_aaaa_records = aws_cache_new_lru(
        new_host_entry->allocator,
        aws_hash_string,
        aws_hash_callback_string_eq,
        NULL,
        on_failed_address_entry_removed,
        config->max_ttl);
    if (AWS_UNLIKELY(!new_host_entry->failed_connection_aaaa_records)) {
        goto setup_host_entry_error;
    }

    if (aws_array_list_init_dynamic(
            &new_host_entry->new_addresses, new_host_entry->allocator, 4, sizeof(struct aws_host_address))) {
        goto setup_host_entry_error;
    }

    if (aws_array_list_init_dynamic(
            &new_host_entry->expired_addresses, new_host_entry->allocator, 4, sizeof(struct aws_host_address))) {
        goto setup_host_entry_error;
    }

    aws_linked_list_init(&new_host_entry->pending_resolution_callbacks);

    pending_callback = aws_mem_acquire(resolver->allocator, sizeof(struct pending_callback));

    if (AWS_UNLIKELY(!pending_callback)) {
        goto setup_host_entry_error;
    }

    /*add the current callback here */
    pending_callback->user_data = user_data;
    pending_callback->callback = res;
    aws_linked_list_push_back(&new_host_entry->pending_resolution_callbacks, &pending_callback->node);

    aws_mutex_init(&new_host_entry->entry_lock);
    new_host_entry->resolution_config = *config;
    aws_condition_variable_init(&new_host_entry->entry_signal);

    aws_thread_init(&new_host_entry->resolver_thread, resolver->allocator);
    thread_init = true;
    struct default_host_resolver *default_host_resolver = resolver->impl;
    if (AWS_UNLIKELY(
            aws_hash_table_put(&default_host_resolver->host_entry_table, host_string_copy, new_host_entry, NULL))) {
        goto setup_host_entry_error;
    }

    struct aws_thread_options thread_options = *aws_default_thread_options();
    thread_options.join_strategy = AWS_TJS_MANAGED;
    thread_options.name = aws_byte_cursor_from_c_str("AwsHostResolver"); /* 15 characters is max for Linux */
    if (aws_thread_launch(
            &new_host_entry->resolver_thread, aws_host_resolver_thread, new_host_entry, &thread_options)) {
        goto setup_host_entry_error;
    }
    ++default_host_resolver->pending_host_entry_shutdown_completion_callbacks;

    return AWS_OP_SUCCESS;

setup_host_entry_error:

    if (thread_init) {
        aws_thread_clean_up(&new_host_entry->resolver_thread);
    }
    // If we registered a callback, clear it. So that we don’t trigger callback and return an error.
    if (!aws_linked_list_empty(&new_host_entry->pending_resolution_callbacks)) {
        aws_linked_list_remove(&pending_callback->node);
    }

    s_clean_up_host_entry(new_host_entry);

    return AWS_OP_ERR;
}

static int default_resolve_host(
    struct aws_host_resolver *resolver,
    const struct aws_string *host_name,
    aws_on_host_resolved_result_fn *res,
    const struct aws_host_resolution_config *config,
    void *user_data) {
    int result = AWS_OP_SUCCESS;

    AWS_LOGF_DEBUG(AWS_LS_IO_DNS, "id=%p: Host resolution requested for %s", (void *)resolver, host_name->bytes);

    uint64_t timestamp = s_get_system_time_for_default_resolver(resolver);

    struct default_host_resolver *default_host_resolver = resolver->impl;
    aws_mutex_lock(&default_host_resolver->resolver_lock);

    struct aws_hash_element *element = NULL;
    /* we don't care about the error code here, only that the host_entry was found or not. */
    aws_hash_table_find(&default_host_resolver->host_entry_table, host_name, &element);

    struct host_entry *host_entry = NULL;
    if (element != NULL) {
        host_entry = element->value;
        AWS_FATAL_ASSERT(host_entry != NULL);
    }

    if (!host_entry) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_DNS,
            "id=%p: No cached entries found for %s starting new resolver thread.",
            (void *)resolver,
            host_name->bytes);

        result = create_and_init_host_entry(resolver, host_name, res, config, timestamp, user_data);
        aws_mutex_unlock(&default_host_resolver->resolver_lock);

        return result;
    }

    aws_mutex_lock(&host_entry->entry_lock);

    /*
     * We don't need to make any resolver side-affects in the remaining logic and it's impossible for the entry
     * to disappear underneath us while holding its lock, so its safe to release the resolver lock and let other
     * things query other entries.
     */
    aws_mutex_unlock(&default_host_resolver->resolver_lock);
    host_entry->last_resolve_request_timestamp_ns = timestamp;
    host_entry->resolves_since_last_request = 0;

    struct aws_host_address_cache_entry *aaaa_entry = aws_lru_cache_use_lru_element(host_entry->aaaa_records);
    struct aws_host_address *aaaa_record = (aaaa_entry != NULL) ? &aaaa_entry->address : NULL;
    struct aws_host_address_cache_entry *a_entry = aws_lru_cache_use_lru_element(host_entry->a_records);
    struct aws_host_address *a_record = (a_entry != NULL) ? &a_entry->address : NULL;

    struct aws_host_address address_array[2];
    AWS_ZERO_ARRAY(address_array);
    struct aws_array_list callback_address_list;
    aws_array_list_init_static(&callback_address_list, address_array, 2, sizeof(struct aws_host_address));

    if ((aaaa_record || a_record)) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_DNS,
            "id=%p: cached entries found for %s returning to caller.",
            (void *)resolver,
            host_name->bytes);

        /* these will all need to be copied so that we don't hold the lock during the callback. */
        if (aaaa_record) {
            struct aws_host_address aaaa_record_cpy;
            aws_host_address_copy(aaaa_record, &aaaa_record_cpy);
            aws_array_list_push_back(&callback_address_list, &aaaa_record_cpy);
            AWS_LOGF_TRACE(
                AWS_LS_IO_DNS,
                "id=%p: vending address %s for host %s to caller",
                (void *)resolver,
                aaaa_record->address->bytes,
                host_entry->host_name->bytes);
        }
        if (a_record) {
            struct aws_host_address a_record_cpy;
            aws_host_address_copy(a_record, &a_record_cpy);
            aws_array_list_push_back(&callback_address_list, &a_record_cpy);
            AWS_LOGF_TRACE(
                AWS_LS_IO_DNS,
                "id=%p: vending address %s for host %s to caller",
                (void *)resolver,
                a_record->address->bytes,
                host_entry->host_name->bytes);
        }
        aws_mutex_unlock(&host_entry->entry_lock);

        /* we don't want to do the callback WHILE we hold the lock someone may reentrantly call us. */
        // TODO: Fire the callback asynchronously
        res(resolver, host_name, AWS_OP_SUCCESS, &callback_address_list, user_data);

        for (size_t i = 0; i < aws_array_list_length(&callback_address_list); ++i) {
            struct aws_host_address *address_ptr = NULL;
            aws_array_list_get_at_ptr(&callback_address_list, (void **)&address_ptr, i);
            aws_host_address_clean_up(address_ptr);
        }

        aws_array_list_clean_up(&callback_address_list);

        return result;
    }

    struct pending_callback *pending_callback =
        aws_mem_acquire(default_host_resolver->allocator, sizeof(struct pending_callback));
    if (pending_callback != NULL) {
        pending_callback->user_data = user_data;
        pending_callback->callback = res;
        aws_linked_list_push_back(&host_entry->pending_resolution_callbacks, &pending_callback->node);
        /*
         * intentionally signal under the lock; similar to the shutdown case, we can't guarantee the resolver
         * is still around once the lock is released.
         */
        aws_condition_variable_notify_all(&host_entry->entry_signal);
    } else {
        result = AWS_OP_ERR;
    }

    aws_mutex_unlock(&host_entry->entry_lock);

    return result;
}

static size_t default_get_host_address_count(
    struct aws_host_resolver *host_resolver,
    const struct aws_string *host_name,
    uint32_t flags) {
    struct default_host_resolver *default_host_resolver = host_resolver->impl;
    size_t address_count = 0;

    aws_mutex_lock(&default_host_resolver->resolver_lock);

    struct aws_hash_element *element = NULL;
    aws_hash_table_find(&default_host_resolver->host_entry_table, host_name, &element);
    if (element != NULL) {
        struct host_entry *host_entry = element->value;
        if (host_entry != NULL) {
            aws_mutex_lock(&host_entry->entry_lock);

            if ((flags & AWS_GET_HOST_ADDRESS_COUNT_RECORD_TYPE_A) != 0) {
                address_count += aws_cache_get_element_count(host_entry->a_records);
            }

            if ((flags & AWS_GET_HOST_ADDRESS_COUNT_RECORD_TYPE_AAAA) != 0) {
                address_count += aws_cache_get_element_count(host_entry->aaaa_records);
            }

            aws_mutex_unlock(&host_entry->entry_lock);
        }
    }

    aws_mutex_unlock(&default_host_resolver->resolver_lock);

    return address_count;
}

static struct aws_host_resolver_vtable s_vtable = {
    .purge_cache = s_resolver_purge_cache,
    .purge_cache_with_callback = s_resolver_purge_cache_with_callback,
    .resolve_host = default_resolve_host,
    .record_connection_failure = resolver_record_connection_failure,
    .get_host_address_count = default_get_host_address_count,
    .destroy = resolver_destroy,
    .purge_host_cache = s_resolver_purge_host_cache,
};

static void s_aws_host_resolver_destroy(struct aws_host_resolver *resolver) {
    AWS_ASSERT(resolver->vtable && resolver->vtable->destroy);
    resolver->vtable->destroy(resolver);
}

struct aws_host_resolver *aws_host_resolver_new_default(
    struct aws_allocator *allocator,
    const struct aws_host_resolver_default_options *options) {
    AWS_FATAL_ASSERT(options != NULL);

    AWS_ASSERT(options->el_group);

    struct aws_host_resolver *resolver = NULL;
    struct default_host_resolver *default_host_resolver = NULL;
    if (!aws_mem_acquire_many(
            allocator,
            2,
            &resolver,
            sizeof(struct aws_host_resolver),
            &default_host_resolver,
            sizeof(struct default_host_resolver))) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*resolver);
    AWS_ZERO_STRUCT(*default_host_resolver);

    AWS_LOGF_INFO(
        AWS_LS_IO_DNS,
        "id=%p: Initializing default host resolver with %llu max host entries.",
        (void *)resolver,
        (unsigned long long)options->max_entries);

    resolver->vtable = &s_vtable;
    resolver->allocator = allocator;
    resolver->impl = default_host_resolver;

    default_host_resolver->event_loop_group = aws_event_loop_group_acquire(options->el_group);
    default_host_resolver->allocator = allocator;
    default_host_resolver->pending_host_entry_shutdown_completion_callbacks = 0;
    default_host_resolver->state = DRS_ACTIVE;
    aws_mutex_init(&default_host_resolver->resolver_lock);

    if (aws_hash_table_init(
            &default_host_resolver->host_entry_table,
            allocator,
            options->max_entries,
            aws_hash_string,
            aws_hash_callback_string_eq,
            NULL,
            NULL)) {
        goto on_error;
    }

    aws_ref_count_init(&resolver->ref_count, resolver, (aws_simple_completion_callback *)s_aws_host_resolver_destroy);

    if (options->shutdown_options != NULL) {
        resolver->shutdown_options = *options->shutdown_options;
    }

    if (options->system_clock_override_fn != NULL) {
        default_host_resolver->system_clock_fn = options->system_clock_override_fn;
    } else {
        default_host_resolver->system_clock_fn = aws_high_res_clock_get_ticks;
    }

    return resolver;

on_error:

    s_cleanup_default_resolver(resolver);

    return NULL;
}

struct aws_host_resolver *aws_host_resolver_acquire(struct aws_host_resolver *resolver) {
    if (resolver != NULL) {
        aws_ref_count_acquire(&resolver->ref_count);
    }

    return resolver;
}

void aws_host_resolver_release(struct aws_host_resolver *resolver) {
    if (resolver != NULL) {
        aws_ref_count_release(&resolver->ref_count);
    }
}

size_t aws_host_resolver_get_host_address_count(
    struct aws_host_resolver *resolver,
    const struct aws_string *host_name,
    uint32_t flags) {
    return resolver->vtable->get_host_address_count(resolver, host_name, flags);
}

struct aws_host_resolution_config aws_host_resolver_init_default_resolution_config(void) {
    struct aws_host_resolution_config config = {
        .impl = aws_default_dns_resolve,
        .max_ttl = AWS_DEFAULT_DNS_TTL,
        .impl_data = NULL,
        .resolve_frequency_ns = NS_PER_SEC,
    };

    return config;
}
