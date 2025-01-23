/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/event_loop.h>

#include <aws/common/shutdown_types.h>
#include <aws/io/private/event_loop_impl.h>

#include <aws/common/clock.h>
#include <aws/common/device_random.h>
#include <aws/common/system_info.h>
#include <aws/common/thread.h>

struct aws_event_loop *aws_event_loop_new_default(struct aws_allocator *alloc, aws_io_clock_fn *clock) {
    struct aws_event_loop_options options = {
        .thread_options = NULL,
        .clock = clock,
    };

    return aws_event_loop_new_default_with_options(alloc, &options);
}

static void s_event_loop_group_thread_exit(void *user_data) {
    struct aws_event_loop_group *el_group = user_data;

    aws_simple_completion_callback *completion_callback = el_group->shutdown_options.shutdown_callback_fn;
    void *completion_user_data = el_group->shutdown_options.shutdown_callback_user_data;

    aws_mem_release(el_group->allocator, el_group);

    if (completion_callback != NULL) {
        completion_callback(completion_user_data);
    }
}

static void s_aws_event_loop_group_shutdown_sync(struct aws_event_loop_group *el_group) {
    while (aws_array_list_length(&el_group->event_loops) > 0) {
        struct aws_event_loop *loop = NULL;

        if (!aws_array_list_back(&el_group->event_loops, &loop)) {
            aws_event_loop_destroy(loop);
        }

        aws_array_list_pop_back(&el_group->event_loops);
    }

    aws_array_list_clean_up(&el_group->event_loops);
}

static void s_event_loop_destroy_async_thread_fn(void *thread_data) {
    struct aws_event_loop_group *el_group = thread_data;

    s_aws_event_loop_group_shutdown_sync(el_group);

    aws_thread_current_at_exit(s_event_loop_group_thread_exit, el_group);
}

static void s_aws_event_loop_group_shutdown_async(struct aws_event_loop_group *el_group) {

    /* It's possible that the last refcount was released on an event-loop thread,
     * so we would deadlock if we waited here for all the event-loop threads to shut down.
     * Therefore, we spawn a NEW thread and have it wait for all the event-loop threads to shut down
     */
    struct aws_thread cleanup_thread;
    AWS_ZERO_STRUCT(cleanup_thread);

    aws_thread_init(&cleanup_thread, el_group->allocator);

    struct aws_thread_options thread_options = *aws_default_thread_options();
    thread_options.join_strategy = AWS_TJS_MANAGED;
    thread_options.name = aws_byte_cursor_from_c_str("EvntLoopCleanup"); /* 15 characters is max for Linux */

    aws_thread_launch(&cleanup_thread, s_event_loop_destroy_async_thread_fn, el_group, &thread_options);
}

struct aws_event_loop_group *aws_event_loop_group_new_internal(
    struct aws_allocator *allocator,
    const struct aws_event_loop_group_options *options,
    aws_new_event_loop_fn *new_loop_fn,
    void *new_loop_user_data) {
    AWS_FATAL_ASSERT(new_loop_fn);

    aws_io_clock_fn *clock = options->clock_override;
    if (!clock) {
        clock = aws_high_res_clock_get_ticks;
    }

    size_t group_cpu_count = 0;
    struct aws_cpu_info *usable_cpus = NULL;

    bool pin_threads = options->cpu_group != NULL;
    if (pin_threads) {
        uint16_t cpu_group = *options->cpu_group;
        group_cpu_count = aws_get_cpu_count_for_group(cpu_group);
        if (!group_cpu_count) {
            // LOG THIS
            aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
            return NULL;
        }

        usable_cpus = aws_mem_calloc(allocator, group_cpu_count, sizeof(struct aws_cpu_info));
        if (usable_cpus == NULL) {
            return NULL;
        }

        aws_get_cpu_ids_for_group(cpu_group, usable_cpus, group_cpu_count);
    }

    struct aws_event_loop_group *el_group = aws_mem_calloc(allocator, 1, sizeof(struct aws_event_loop_group));
    if (el_group == NULL) {
        return NULL;
    }

    el_group->allocator = allocator;
    aws_ref_count_init(
        &el_group->ref_count, el_group, (aws_simple_completion_callback *)s_aws_event_loop_group_shutdown_async);

    uint16_t el_count = options->loop_count;
    if (el_count == 0) {
        uint16_t processor_count = (uint16_t)aws_system_info_processor_count();
        /* cut them in half to avoid using hyper threads for the IO work. */
        el_count = processor_count > 1 ? processor_count / 2 : processor_count;
    }

    if (aws_array_list_init_dynamic(&el_group->event_loops, allocator, el_count, sizeof(struct aws_event_loop *))) {
        goto on_error;
    }

    for (uint16_t i = 0; i < el_count; ++i) {
        /* Don't pin to hyper-threads if a user cared enough to specify a NUMA node */
        if (!pin_threads || (i < group_cpu_count && !usable_cpus[i].suspected_hyper_thread)) {
            struct aws_thread_options thread_options = *aws_default_thread_options();

            struct aws_event_loop_options el_options = {
                .clock = clock,
                .thread_options = &thread_options,
            };

            if (pin_threads) {
                thread_options.cpu_id = usable_cpus[i].cpu_id;
            }

            /* Thread name should be <= 15 characters */
            char thread_name[32] = {0};
            int thread_name_len = snprintf(thread_name, sizeof(thread_name), "AwsEventLoop %d", (int)i + 1);
            if (thread_name_len > AWS_THREAD_NAME_RECOMMENDED_STRLEN) {
                snprintf(thread_name, sizeof(thread_name), "AwsEventLoop");
            }
            thread_options.name = aws_byte_cursor_from_c_str(thread_name);

            struct aws_event_loop *loop = new_loop_fn(allocator, &el_options, new_loop_user_data);
            if (!loop) {
                goto on_error;
            }

            if (aws_array_list_push_back(&el_group->event_loops, (const void *)&loop)) {
                aws_event_loop_destroy(loop);
                goto on_error;
            }

            if (aws_event_loop_run(loop)) {
                goto on_error;
            }
        }
    }

    if (options->shutdown_options != NULL) {
        el_group->shutdown_options = *options->shutdown_options;
    }

    if (pin_threads) {
        aws_mem_release(allocator, usable_cpus);
    }

    return el_group;

on_error:;
    /* cache the error code to prevent any potential side effects */
    int cached_error_code = aws_last_error();

    aws_mem_release(allocator, usable_cpus);
    s_aws_event_loop_group_shutdown_sync(el_group);
    s_event_loop_group_thread_exit(el_group);

    /* raise the cached error code */
    aws_raise_error(cached_error_code);
    return NULL;
}

static struct aws_event_loop *s_default_new_event_loop(
    struct aws_allocator *allocator,
    const struct aws_event_loop_options *options,
    void *user_data) {

    (void)user_data;
    return aws_event_loop_new_default_with_options(allocator, options);
}

struct aws_event_loop_group *aws_event_loop_group_new(
    struct aws_allocator *allocator,
    const struct aws_event_loop_group_options *options) {

    return aws_event_loop_group_new_internal(allocator, options, s_default_new_event_loop, NULL);
}

struct aws_event_loop_group *aws_event_loop_group_acquire(struct aws_event_loop_group *el_group) {
    if (el_group != NULL) {
        aws_ref_count_acquire(&el_group->ref_count);
    }

    return el_group;
}

void aws_event_loop_group_release(struct aws_event_loop_group *el_group) {
    if (el_group != NULL) {
        aws_ref_count_release(&el_group->ref_count);
    }
}

size_t aws_event_loop_group_get_loop_count(const struct aws_event_loop_group *el_group) {
    return aws_array_list_length(&el_group->event_loops);
}

struct aws_event_loop *aws_event_loop_group_get_loop_at(struct aws_event_loop_group *el_group, size_t index) {
    struct aws_event_loop *el = NULL;
    aws_array_list_get_at(&el_group->event_loops, &el, index);
    return el;
}

struct aws_event_loop *aws_event_loop_group_get_next_loop(struct aws_event_loop_group *el_group) {
    size_t loop_count = aws_array_list_length(&el_group->event_loops);
    AWS_ASSERT(loop_count > 0);
    if (loop_count == 0) {
        return NULL;
    }

    /* do one call to get 32 random bits because this hits an actual entropy source and it's not cheap */
    uint32_t random_32_bit_num = 0;
    aws_device_random_u32(&random_32_bit_num);

    /* use the best of two algorithm to select the loop with the lowest load.
     * If we find device random is too hard on the kernel, we can seed it and use another random
     * number generator. */

    /* it's fine and intentional, the case will throw off the top 16 bits and that's what we want. */
    uint16_t random_num_a = (uint16_t)random_32_bit_num;
    random_num_a = random_num_a % loop_count;

    uint16_t random_num_b = (uint16_t)(random_32_bit_num >> 16);
    random_num_b = random_num_b % loop_count;

    struct aws_event_loop *random_loop_a = NULL;
    struct aws_event_loop *random_loop_b = NULL;
    aws_array_list_get_at(&el_group->event_loops, &random_loop_a, random_num_a);
    aws_array_list_get_at(&el_group->event_loops, &random_loop_b, random_num_b);

    /* there's no logical reason why this should ever be possible. It's just best to die if it happens. */
    AWS_FATAL_ASSERT((random_loop_a && random_loop_b) && "random_loop_a or random_loop_b is NULL.");

    size_t load_a = aws_event_loop_get_load_factor(random_loop_a);
    size_t load_b = aws_event_loop_get_load_factor(random_loop_b);

    return load_a < load_b ? random_loop_a : random_loop_b;
}

static void s_object_removed(void *value) {
    struct aws_event_loop_local_object *object = (struct aws_event_loop_local_object *)value;
    if (object->on_object_removed) {
        object->on_object_removed(object);
    }
}

int aws_event_loop_init_base(struct aws_event_loop *event_loop, struct aws_allocator *alloc, aws_io_clock_fn *clock) {
    AWS_ZERO_STRUCT(*event_loop);

    event_loop->alloc = alloc;
    event_loop->clock = clock;
    aws_atomic_init_int(&event_loop->current_load_factor, 0u);
    aws_atomic_init_int(&event_loop->next_flush_time, 0u);

    if (aws_hash_table_init(&event_loop->local_data, alloc, 20, aws_hash_ptr, aws_ptr_eq, NULL, s_object_removed)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

void aws_event_loop_clean_up_base(struct aws_event_loop *event_loop) {
    aws_hash_table_clean_up(&event_loop->local_data);
}

void aws_event_loop_register_tick_start(struct aws_event_loop *event_loop) {
    aws_high_res_clock_get_ticks(&event_loop->latest_tick_start);
}

void aws_event_loop_register_tick_end(struct aws_event_loop *event_loop) {
    /* increment the timestamp diff counter (this should always be called from the same thread), the concurrency
     * work happens during the flush. */
    uint64_t end_tick = 0;
    aws_high_res_clock_get_ticks(&end_tick);

    size_t elapsed = (size_t)aws_min_u64(end_tick - event_loop->latest_tick_start, SIZE_MAX);
    event_loop->current_tick_latency_sum = aws_add_size_saturating(event_loop->current_tick_latency_sum, elapsed);
    event_loop->latest_tick_start = 0;

    size_t next_flush_time_secs = aws_atomic_load_int(&event_loop->next_flush_time);
    /* store as seconds because we can't make a 64-bit integer reliably atomic across platforms. */
    uint64_t end_tick_secs = aws_timestamp_convert(end_tick, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, NULL);

    /* if a second has passed, flush the load-factor. */
    if (end_tick_secs > next_flush_time_secs) {
        aws_atomic_store_int(&event_loop->current_load_factor, event_loop->current_tick_latency_sum);
        event_loop->current_tick_latency_sum = 0;
        /* run again in a second. */
        aws_atomic_store_int(&event_loop->next_flush_time, (size_t)(end_tick_secs + 1));
    }
}

size_t aws_event_loop_get_load_factor(struct aws_event_loop *event_loop) {
    uint64_t current_time = 0;
    aws_high_res_clock_get_ticks(&current_time);

    uint64_t current_time_secs = aws_timestamp_convert(current_time, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, NULL);
    size_t next_flush_time_secs = aws_atomic_load_int(&event_loop->next_flush_time);

    /* safety valve just in case an event-loop had heavy load and then went completely idle. If we haven't
     * had an update from the event-loop in 10 seconds, just assume idle. Also, yes this is racy, but it should
     * be good enough because an active loop will be updating its counter frequently ( more than once per 10 seconds
     * for sure ), in the case where we hit the technical race condition, we don't care anyways and returning 0
     * is the desired behavior. */
    if (current_time_secs > next_flush_time_secs + 10) {
        return 0;
    }

    return aws_atomic_load_int(&event_loop->current_load_factor);
}

void aws_event_loop_destroy(struct aws_event_loop *event_loop) {
    if (!event_loop) {
        return;
    }

    AWS_ASSERT(event_loop->vtable && event_loop->vtable->destroy);
    AWS_ASSERT(!aws_event_loop_thread_is_callers_thread(event_loop));

    event_loop->vtable->destroy(event_loop);
}

int aws_event_loop_fetch_local_object(
    struct aws_event_loop *event_loop,
    void *key,
    struct aws_event_loop_local_object *obj) {

    AWS_ASSERT(aws_event_loop_thread_is_callers_thread(event_loop));

    struct aws_hash_element *object = NULL;
    if (!aws_hash_table_find(&event_loop->local_data, key, &object) && object) {
        *obj = *(struct aws_event_loop_local_object *)object->value;
        return AWS_OP_SUCCESS;
    }

    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}

int aws_event_loop_put_local_object(struct aws_event_loop *event_loop, struct aws_event_loop_local_object *obj) {
    AWS_ASSERT(aws_event_loop_thread_is_callers_thread(event_loop));

    struct aws_hash_element *object = NULL;
    int was_created = 0;

    if (!aws_hash_table_create(&event_loop->local_data, obj->key, &object, &was_created)) {
        object->key = obj->key;
        object->value = obj;
        return AWS_OP_SUCCESS;
    }

    return AWS_OP_ERR;
}

int aws_event_loop_remove_local_object(
    struct aws_event_loop *event_loop,
    void *key,
    struct aws_event_loop_local_object *removed_obj) {

    AWS_ASSERT(aws_event_loop_thread_is_callers_thread(event_loop));

    struct aws_hash_element existing_object;
    AWS_ZERO_STRUCT(existing_object);

    int was_present = 0;

    struct aws_hash_element *remove_candidate = removed_obj ? &existing_object : NULL;

    if (!aws_hash_table_remove(&event_loop->local_data, key, remove_candidate, &was_present)) {
        if (remove_candidate && was_present) {
            *removed_obj = *(struct aws_event_loop_local_object *)existing_object.value;
        }

        return AWS_OP_SUCCESS;
    }

    return AWS_OP_ERR;
}

int aws_event_loop_run(struct aws_event_loop *event_loop) {
    AWS_ASSERT(event_loop->vtable && event_loop->vtable->run);
    return event_loop->vtable->run(event_loop);
}

int aws_event_loop_stop(struct aws_event_loop *event_loop) {
    AWS_ASSERT(event_loop->vtable && event_loop->vtable->stop);
    return event_loop->vtable->stop(event_loop);
}

int aws_event_loop_wait_for_stop_completion(struct aws_event_loop *event_loop) {
    AWS_ASSERT(!aws_event_loop_thread_is_callers_thread(event_loop));
    AWS_ASSERT(event_loop->vtable && event_loop->vtable->wait_for_stop_completion);
    return event_loop->vtable->wait_for_stop_completion(event_loop);
}

void aws_event_loop_schedule_task_now(struct aws_event_loop *event_loop, struct aws_task *task) {
    AWS_ASSERT(event_loop->vtable && event_loop->vtable->schedule_task_now);
    AWS_ASSERT(task);
    event_loop->vtable->schedule_task_now(event_loop, task);
}

void aws_event_loop_schedule_task_future(
    struct aws_event_loop *event_loop,
    struct aws_task *task,
    uint64_t run_at_nanos) {

    AWS_ASSERT(event_loop->vtable && event_loop->vtable->schedule_task_future);
    AWS_ASSERT(task);
    event_loop->vtable->schedule_task_future(event_loop, task, run_at_nanos);
}

void aws_event_loop_cancel_task(struct aws_event_loop *event_loop, struct aws_task *task) {
    AWS_ASSERT(event_loop->vtable && event_loop->vtable->cancel_task);
    AWS_ASSERT(aws_event_loop_thread_is_callers_thread(event_loop));
    AWS_ASSERT(task);
    event_loop->vtable->cancel_task(event_loop, task);
}

#if AWS_USE_IO_COMPLETION_PORTS

int aws_event_loop_connect_handle_to_io_completion_port(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle) {

    AWS_ASSERT(event_loop->vtable && event_loop->vtable->connect_to_io_completion_port);
    return event_loop->vtable->connect_to_io_completion_port(event_loop, handle);
}

#else  /* !AWS_USE_IO_COMPLETION_PORTS */

int aws_event_loop_subscribe_to_io_events(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    aws_event_loop_on_event_fn *on_event,
    void *user_data) {

    AWS_ASSERT(event_loop->vtable && event_loop->vtable->subscribe_to_io_events);
    return event_loop->vtable->subscribe_to_io_events(event_loop, handle, events, on_event, user_data);
}
#endif /* AWS_USE_IO_COMPLETION_PORTS */

int aws_event_loop_unsubscribe_from_io_events(struct aws_event_loop *event_loop, struct aws_io_handle *handle) {
    AWS_ASSERT(aws_event_loop_thread_is_callers_thread(event_loop));
    AWS_ASSERT(event_loop->vtable && event_loop->vtable->unsubscribe_from_io_events);
    return event_loop->vtable->unsubscribe_from_io_events(event_loop, handle);
}

void aws_event_loop_free_io_event_resources(struct aws_event_loop *event_loop, struct aws_io_handle *handle) {
    AWS_ASSERT(event_loop && event_loop->vtable->free_io_event_resources);
    event_loop->vtable->free_io_event_resources(handle->additional_data);
}

bool aws_event_loop_thread_is_callers_thread(struct aws_event_loop *event_loop) {
    AWS_ASSERT(event_loop->vtable && event_loop->vtable->is_on_callers_thread);
    return event_loop->vtable->is_on_callers_thread(event_loop);
}

int aws_event_loop_current_clock_time(const struct aws_event_loop *event_loop, uint64_t *time_nanos) {
    AWS_ASSERT(event_loop->clock);
    return event_loop->clock(time_nanos);
}

struct aws_event_loop_group *aws_event_loop_group_new_default(
    struct aws_allocator *alloc,
    uint16_t max_threads,
    const struct aws_shutdown_callback_options *shutdown_options) {

    struct aws_event_loop_group_options elg_options = {
        .loop_count = max_threads,
        .shutdown_options = shutdown_options,
    };

    return aws_event_loop_group_new(alloc, &elg_options);
}

struct aws_event_loop_group *aws_event_loop_group_new_default_pinned_to_cpu_group(
    struct aws_allocator *alloc,
    uint16_t max_threads,
    uint16_t cpu_group,
    const struct aws_shutdown_callback_options *shutdown_options) {

    struct aws_event_loop_group_options elg_options = {
        .loop_count = max_threads,
        .shutdown_options = shutdown_options,
        .cpu_group = &cpu_group,
    };

    return aws_event_loop_group_new(alloc, &elg_options);
}

void *aws_event_loop_get_impl(struct aws_event_loop *event_loop) {
    return event_loop->impl_data;
}

struct aws_event_loop *aws_event_loop_new_base(
    struct aws_allocator *allocator,
    aws_io_clock_fn *clock,
    struct aws_event_loop_vtable *vtable,
    void *impl) {
    struct aws_event_loop *event_loop = aws_mem_acquire(allocator, sizeof(struct aws_event_loop));
    aws_event_loop_init_base(event_loop, allocator, clock);
    event_loop->impl_data = impl;
    event_loop->vtable = vtable;

    return event_loop;
}
