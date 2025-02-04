/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/atomics.h>
#include <aws/common/byte_buf.h>
#include <aws/common/clock.h>
#include <aws/common/hash_table.h>
#include <aws/common/logging.h>
#include <aws/common/mutex.h>
#include <aws/common/priority_queue.h>
#include <aws/common/string.h>
#include <aws/common/system_info.h>

/* describes a single live allocation.
 * allocated by aws_default_allocator() */
struct alloc_info {
    size_t size;
    uint64_t time;
    uint64_t stack; /* hash of stack frame pointers */
};

/* Using a flexible array member is the C99 compliant way to have the frames immediately follow the header.
 *
 * MSVC doesn't know this for some reason so we need to use a pragma to make
 * it happy.
 */
#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4200) /* nonstandard extension used: zero-sized array in struct/union */
#endif

/* one of these is stored per unique stack
 * allocated by aws_default_allocator() */
struct stack_trace {
    size_t depth;         /* length of frames[] */
    void *const frames[]; /* rest of frames are allocated after */
};

#ifdef _MSC_VER
#    pragma warning(pop)
#endif

/* Tracking structure, used as the allocator impl.
 * This structure, and all its bookkeeping data structures, are created with the aws_default_allocator().
 * This is not customizable because it's too expensive for every little allocation to store
 * a pointer back to its original allocator. */
struct alloc_tracer {
    struct aws_allocator *traced_allocator; /* underlying allocator */
    enum aws_mem_trace_level level;         /* level to trace at */
    size_t frames_per_stack;                /* how many frames to keep per stack */
    struct aws_atomic_var allocated;        /* bytes currently allocated */
    struct aws_mutex mutex;                 /* protects everything below */
    struct aws_hash_table allocs;           /* live allocations, maps address -> alloc_info */
    struct aws_hash_table stacks;           /* unique stack traces, maps hash -> stack_trace */
};

/* number of frames to skip in call stacks (s_alloc_tracer_track, and the vtable function) */
enum { FRAMES_TO_SKIP = 2 };

static void *s_trace_mem_acquire(struct aws_allocator *allocator, size_t size);
static void s_trace_mem_release(struct aws_allocator *allocator, void *ptr);
static void *s_trace_mem_realloc(struct aws_allocator *allocator, void *old_ptr, size_t old_size, size_t new_size);
static void *s_trace_mem_calloc(struct aws_allocator *allocator, size_t num, size_t size);

static struct aws_allocator s_trace_allocator = {
    .mem_acquire = s_trace_mem_acquire,
    .mem_release = s_trace_mem_release,
    .mem_realloc = s_trace_mem_realloc,
    .mem_calloc = s_trace_mem_calloc,
};

/* for the hash table, to destroy elements */
static void s_destroy_alloc(void *data) {
    struct alloc_info *alloc = data;
    aws_mem_release(aws_default_allocator(), alloc);
}

static void s_destroy_stacktrace(void *data) {
    struct stack_trace *stack = data;
    aws_mem_release(aws_default_allocator(), stack);
}

static void s_alloc_tracer_init(
    struct alloc_tracer *tracer,
    struct aws_allocator *traced_allocator,
    enum aws_mem_trace_level level,
    size_t frames_per_stack) {

    void *stack[1];
    if (!aws_backtrace(stack, 1)) {
        /* clamp level if tracing isn't available */
        level = level > AWS_MEMTRACE_BYTES ? AWS_MEMTRACE_BYTES : level;
    }

    tracer->traced_allocator = traced_allocator;
    tracer->level = level;

    if (tracer->level >= AWS_MEMTRACE_BYTES) {
        aws_atomic_init_int(&tracer->allocated, 0);
        AWS_FATAL_ASSERT(AWS_OP_SUCCESS == aws_mutex_init(&tracer->mutex));
        AWS_FATAL_ASSERT(
            AWS_OP_SUCCESS ==
            aws_hash_table_init(
                &tracer->allocs, aws_default_allocator(), 1024, aws_hash_ptr, aws_ptr_eq, NULL, s_destroy_alloc));
    }

    if (tracer->level == AWS_MEMTRACE_STACKS) {
        if (frames_per_stack > 128) {
            frames_per_stack = 128;
        }
        tracer->frames_per_stack = frames_per_stack ? frames_per_stack : 8;
        AWS_FATAL_ASSERT(
            AWS_OP_SUCCESS ==
            aws_hash_table_init(
                &tracer->stacks, aws_default_allocator(), 1024, aws_hash_ptr, aws_ptr_eq, NULL, s_destroy_stacktrace));
    }
}

static void s_alloc_tracer_track(struct alloc_tracer *tracer, void *ptr, size_t size) {
    if (tracer->level == AWS_MEMTRACE_NONE) {
        return;
    }

    aws_atomic_fetch_add(&tracer->allocated, size);

    struct alloc_info *alloc = aws_mem_calloc(aws_default_allocator(), 1, sizeof(struct alloc_info));
    AWS_FATAL_ASSERT(alloc);
    alloc->size = size;
    aws_high_res_clock_get_ticks(&alloc->time);

    if (tracer->level == AWS_MEMTRACE_STACKS) {
        /* capture stack frames,
         * skip 2 for this function and the allocation vtable function if we have a full stack trace
         * and otherwise just capture what ever stack trace we got
         */
        AWS_VARIABLE_LENGTH_ARRAY(void *, stack_frames, (FRAMES_TO_SKIP + tracer->frames_per_stack));
        size_t stack_depth = aws_backtrace(stack_frames, FRAMES_TO_SKIP + tracer->frames_per_stack);
        AWS_FATAL_ASSERT(stack_depth > 0);

        /* hash the stack pointers */
        struct aws_byte_cursor stack_cursor = aws_byte_cursor_from_array(stack_frames, stack_depth * sizeof(void *));
        uint64_t stack_id = aws_hash_byte_cursor_ptr(&stack_cursor);
        alloc->stack = stack_id; /* associate the stack with the alloc */

        aws_mutex_lock(&tracer->mutex);
        struct aws_hash_element *item = NULL;
        int was_created = 0;
        AWS_FATAL_ASSERT(
            AWS_OP_SUCCESS == aws_hash_table_create(&tracer->stacks, (void *)(uintptr_t)stack_id, &item, &was_created));
        /* If this is a new stack, save it to the hash */
        if (was_created) {
            struct stack_trace *stack = aws_mem_calloc(
                aws_default_allocator(), 1, sizeof(struct stack_trace) + (sizeof(void *) * tracer->frames_per_stack));
            AWS_FATAL_ASSERT(stack);
            /**
             * Optimizations can affect the number of frames we get and in pathological cases we can
             * get fewer than FRAMES_TO_SKIP frames, but always at least 1 because code has to start somewhere.
             * (looking at you gcc with -O3 on aarch64)
             * With optimizations on we cannot trust the stack trace too much.
             * Memtracer makes an assumption that stack trace will be available in all cases if stack trace api
             * works. So in the pathological case of stack_depth <= FRAMES_TO_SKIP lets record all the frames we
             * have, to at least have an anchor for where allocation is comming from, however inaccurate it is.
             */
            if (stack_depth <= FRAMES_TO_SKIP) {
                memcpy((void **)&stack->frames[0], &stack_frames[0], (stack_depth) * sizeof(void *));
                stack->depth = stack_depth;
                item->value = stack;
            } else {
                memcpy(
                    (void **)&stack->frames[0],
                    &stack_frames[FRAMES_TO_SKIP],
                    (stack_depth - FRAMES_TO_SKIP) * sizeof(void *));
                stack->depth = stack_depth - FRAMES_TO_SKIP;
                item->value = stack;
            }
        }

        aws_mutex_unlock(&tracer->mutex);
    }

    aws_mutex_lock(&tracer->mutex);
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == aws_hash_table_put(&tracer->allocs, ptr, alloc, NULL));
    aws_mutex_unlock(&tracer->mutex);
}

static void s_alloc_tracer_untrack(struct alloc_tracer *tracer, void *ptr) {
    if (tracer->level == AWS_MEMTRACE_NONE) {
        return;
    }

    aws_mutex_lock(&tracer->mutex);
    struct aws_hash_element *item;
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == aws_hash_table_find(&tracer->allocs, ptr, &item));
    /* because the tracer can be installed at any time, it is possible for an allocation to not
     * be tracked. Therefore, we make sure the find succeeds, but then check the returned
     * value */
    if (item) {
        AWS_FATAL_ASSERT(item->key == ptr && item->value);
        struct alloc_info *alloc = item->value;
        aws_atomic_fetch_sub(&tracer->allocated, alloc->size);
        s_destroy_alloc(item->value);
        AWS_FATAL_ASSERT(AWS_OP_SUCCESS == aws_hash_table_remove_element(&tracer->allocs, item));
    }
    aws_mutex_unlock(&tracer->mutex);
}

/* used only to resolve stacks -> trace, count, size at dump time */
struct stack_metadata {
    struct aws_string *trace;
    size_t count;
    size_t size;
};

static int s_collect_stack_trace(void *context, struct aws_hash_element *item) {
    struct alloc_tracer *tracer = context;
    struct aws_hash_table *all_stacks = &tracer->stacks;
    struct stack_metadata *stack_info = item->value;
    struct aws_hash_element *stack_item = NULL;
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == aws_hash_table_find(all_stacks, item->key, &stack_item));
    AWS_FATAL_ASSERT(stack_item);
    struct stack_trace *stack = stack_item->value;
    void *const *stack_frames = &stack->frames[0];

    /* convert the frame pointers to symbols, and concat into a buffer */
    char buf[4096] = {0};
    struct aws_byte_buf stacktrace = aws_byte_buf_from_empty_array(buf, AWS_ARRAY_SIZE(buf));
    struct aws_byte_cursor newline = aws_byte_cursor_from_c_str("\n");
    char **symbols = aws_backtrace_symbols(stack_frames, stack->depth);
    for (size_t idx = 0; idx < stack->depth; ++idx) {
        if (idx > 0) {
            aws_byte_buf_append(&stacktrace, &newline);
        }
        const char *caller = symbols[idx];
        if (!caller || !caller[0]) {
            break;
        }
        struct aws_byte_cursor cursor = aws_byte_cursor_from_c_str(caller);
        aws_byte_buf_append(&stacktrace, &cursor);
    }
    aws_mem_release(aws_default_allocator(), symbols);
    /* record the resultant buffer as a string */
    stack_info->trace = aws_string_new_from_array(aws_default_allocator(), stacktrace.buffer, stacktrace.len);
    AWS_FATAL_ASSERT(stack_info->trace);
    aws_byte_buf_clean_up(&stacktrace);
    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static int s_stack_info_compare_size(const void *a, const void *b) {
    const struct stack_metadata *stack_a = *(const struct stack_metadata **)a;
    const struct stack_metadata *stack_b = *(const struct stack_metadata **)b;
    return stack_b->size > stack_a->size;
}

static int s_stack_info_compare_count(const void *a, const void *b) {
    const struct stack_metadata *stack_a = *(const struct stack_metadata **)a;
    const struct stack_metadata *stack_b = *(const struct stack_metadata **)b;
    return stack_b->count > stack_a->count;
}

static void s_stack_info_destroy(void *data) {
    struct stack_metadata *stack = data;
    struct aws_allocator *allocator = stack->trace->allocator;
    aws_string_destroy(stack->trace);
    aws_mem_release(allocator, stack);
}

/* tally up count/size per stack from all allocs */
static int s_collect_stack_stats(void *context, struct aws_hash_element *item) {
    struct aws_hash_table *stack_info = context;
    struct alloc_info *alloc = item->value;
    struct aws_hash_element *stack_item = NULL;
    int was_created = 0;
    AWS_FATAL_ASSERT(
        AWS_OP_SUCCESS ==
        aws_hash_table_create(stack_info, (void *)(uintptr_t)alloc->stack, &stack_item, &was_created));
    if (was_created) {
        stack_item->value = aws_mem_calloc(aws_default_allocator(), 1, sizeof(struct stack_metadata));
        AWS_FATAL_ASSERT(stack_item->value);
    }
    struct stack_metadata *stack = stack_item->value;
    stack->count++;
    stack->size += alloc->size;
    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static int s_insert_stacks(void *context, struct aws_hash_element *item) {
    struct aws_priority_queue *pq = context;
    struct stack_metadata *stack = item->value;
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == aws_priority_queue_push(pq, &stack));
    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static int s_insert_allocs(void *context, struct aws_hash_element *item) {
    struct aws_priority_queue *allocs = context;
    struct alloc_info *alloc = item->value;
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == aws_priority_queue_push(allocs, &alloc));
    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static int s_alloc_compare(const void *a, const void *b) {
    const struct alloc_info *alloc_a = *(const struct alloc_info **)a;
    const struct alloc_info *alloc_b = *(const struct alloc_info **)b;
    return alloc_a->time > alloc_b->time;
}

void aws_mem_tracer_dump(struct aws_allocator *trace_allocator) {
    struct alloc_tracer *tracer = trace_allocator->impl;
    if (tracer->level == AWS_MEMTRACE_NONE || aws_atomic_load_int(&tracer->allocated) == 0) {
        return;
    }

    aws_mutex_lock(&tracer->mutex);

    size_t num_allocs = aws_hash_table_get_entry_count(&tracer->allocs);
    AWS_LOGF_TRACE(
        AWS_LS_COMMON_MEMTRACE, "################################################################################");
    AWS_LOGF_TRACE(
        AWS_LS_COMMON_MEMTRACE, "#  BEGIN MEMTRACE DUMP                                                         #");
    AWS_LOGF_TRACE(
        AWS_LS_COMMON_MEMTRACE, "################################################################################");
    AWS_LOGF_TRACE(
        AWS_LS_COMMON_MEMTRACE,
        "tracer: %zu bytes still allocated in %zu allocations",
        aws_atomic_load_int(&tracer->allocated),
        num_allocs);

    /* convert stacks from pointers -> symbols */
    struct aws_hash_table stack_info;
    AWS_ZERO_STRUCT(stack_info);
    if (tracer->level == AWS_MEMTRACE_STACKS) {
        AWS_FATAL_ASSERT(
            AWS_OP_SUCCESS ==
            aws_hash_table_init(
                &stack_info, aws_default_allocator(), 64, aws_hash_ptr, aws_ptr_eq, NULL, s_stack_info_destroy));
        /* collect active stacks, tally up sizes and counts */
        aws_hash_table_foreach(&tracer->allocs, s_collect_stack_stats, &stack_info);
        /* collect stack traces for active stacks */
        aws_hash_table_foreach(&stack_info, s_collect_stack_trace, tracer);
    }

    /* sort allocs by time */
    struct aws_priority_queue allocs;
    AWS_FATAL_ASSERT(
        AWS_OP_SUCCESS ==
        aws_priority_queue_init_dynamic(
            &allocs, aws_default_allocator(), num_allocs, sizeof(struct alloc_info *), s_alloc_compare));
    aws_hash_table_foreach(&tracer->allocs, s_insert_allocs, &allocs);
    /* dump allocs by time */
    AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "Leaks in order of allocation:");
    AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    while (aws_priority_queue_size(&allocs)) {
        struct alloc_info *alloc = NULL;
        aws_priority_queue_pop(&allocs, &alloc);
        if (alloc->stack) {
            struct aws_hash_element *item = NULL;
            AWS_FATAL_ASSERT(
                AWS_OP_SUCCESS == aws_hash_table_find(&stack_info, (void *)(uintptr_t)alloc->stack, &item));
            struct stack_metadata *stack = item->value;
            AWS_LOGF_TRACE(
                AWS_LS_COMMON_MEMTRACE,
                "ALLOC %zu bytes, stacktrace:\n%s\n",
                alloc->size,
                aws_string_c_str(stack->trace));
        } else {
            AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "ALLOC %zu bytes", alloc->size);
        }
    }

    aws_priority_queue_clean_up(&allocs);

    if (tracer->level == AWS_MEMTRACE_STACKS) {
        size_t num_stacks = aws_hash_table_get_entry_count(&stack_info);
        /* sort stacks by total size leaked */
        struct aws_priority_queue stacks_by_size;
        AWS_FATAL_ASSERT(
            AWS_OP_SUCCESS == aws_priority_queue_init_dynamic(
                                  &stacks_by_size,
                                  aws_default_allocator(),
                                  num_stacks,
                                  sizeof(struct stack_metadata *),
                                  s_stack_info_compare_size));
        aws_hash_table_foreach(&stack_info, s_insert_stacks, &stacks_by_size);
        AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
        AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "Stacks by bytes leaked:");
        AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
        while (aws_priority_queue_size(&stacks_by_size) > 0) {
            struct stack_metadata *stack = NULL;
            aws_priority_queue_pop(&stacks_by_size, &stack);
            AWS_LOGF_TRACE(
                AWS_LS_COMMON_MEMTRACE,
                "%zu bytes in %zu allocations:\n%s\n",
                stack->size,
                stack->count,
                aws_string_c_str(stack->trace));
        }
        aws_priority_queue_clean_up(&stacks_by_size);

        /* sort stacks by number of leaks */
        struct aws_priority_queue stacks_by_count;
        AWS_FATAL_ASSERT(
            AWS_OP_SUCCESS == aws_priority_queue_init_dynamic(
                                  &stacks_by_count,
                                  aws_default_allocator(),
                                  num_stacks,
                                  sizeof(struct stack_metadata *),
                                  s_stack_info_compare_count));
        AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
        AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "Stacks by number of leaks:");
        AWS_LOGF_TRACE(AWS_LS_COMMON_MEMTRACE, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
        aws_hash_table_foreach(&stack_info, s_insert_stacks, &stacks_by_count);
        while (aws_priority_queue_size(&stacks_by_count) > 0) {
            struct stack_metadata *stack = NULL;
            aws_priority_queue_pop(&stacks_by_count, &stack);
            AWS_LOGF_TRACE(
                AWS_LS_COMMON_MEMTRACE,
                "%zu allocations leaking %zu bytes:\n%s\n",
                stack->count,
                stack->size,
                aws_string_c_str(stack->trace));
        }
        aws_priority_queue_clean_up(&stacks_by_count);
        aws_hash_table_clean_up(&stack_info);
    }

    AWS_LOGF_TRACE(
        AWS_LS_COMMON_MEMTRACE, "################################################################################");
    AWS_LOGF_TRACE(
        AWS_LS_COMMON_MEMTRACE, "#  END MEMTRACE DUMP                                                           #");
    AWS_LOGF_TRACE(
        AWS_LS_COMMON_MEMTRACE, "################################################################################");

    aws_mutex_unlock(&tracer->mutex);
}

static void *s_trace_mem_acquire(struct aws_allocator *allocator, size_t size) {
    struct alloc_tracer *tracer = allocator->impl;
    void *ptr = aws_mem_acquire(tracer->traced_allocator, size);
    if (ptr) {
        s_alloc_tracer_track(tracer, ptr, size);
    }
    return ptr;
}

static void s_trace_mem_release(struct aws_allocator *allocator, void *ptr) {
    struct alloc_tracer *tracer = allocator->impl;
    s_alloc_tracer_untrack(tracer, ptr);
    aws_mem_release(tracer->traced_allocator, ptr);
}

static void *s_trace_mem_realloc(struct aws_allocator *allocator, void *old_ptr, size_t old_size, size_t new_size) {
    struct alloc_tracer *tracer = allocator->impl;
    void *new_ptr = old_ptr;

    /*
     * Careful with the ordering of state clean up here.
     * Tracer keeps a hash table (alloc ptr as key) of meta info about each allocation.
     * To avoid race conditions during realloc state update needs to be done in
     * following order to avoid race conditions:
     * - remove meta info (other threads cant reuse that key, cause ptr is still valid )
     * - realloc (cant fail, ptr might remain the same)
     * - add meta info for reallocated mem
     */
    s_alloc_tracer_untrack(tracer, old_ptr);
    aws_mem_realloc(tracer->traced_allocator, &new_ptr, old_size, new_size);
    s_alloc_tracer_track(tracer, new_ptr, new_size);

    return new_ptr;
}

static void *s_trace_mem_calloc(struct aws_allocator *allocator, size_t num, size_t size) {
    struct alloc_tracer *tracer = allocator->impl;
    void *ptr = aws_mem_calloc(tracer->traced_allocator, num, size);
    if (ptr) {
        s_alloc_tracer_track(tracer, ptr, num * size);
    }
    return ptr;
}

struct aws_allocator *aws_mem_tracer_new(
    struct aws_allocator *allocator,
    struct aws_allocator *deprecated,
    enum aws_mem_trace_level level,
    size_t frames_per_stack) {

    /* deprecated customizable bookkeeping allocator */
    (void)deprecated;

    struct alloc_tracer *tracer = NULL;
    struct aws_allocator *trace_allocator = NULL;
    aws_mem_acquire_many(
        aws_default_allocator(),
        2,
        &tracer,
        sizeof(struct alloc_tracer),
        &trace_allocator,
        sizeof(struct aws_allocator));

    AWS_FATAL_ASSERT(trace_allocator);
    AWS_FATAL_ASSERT(tracer);

    AWS_ZERO_STRUCT(*trace_allocator);
    AWS_ZERO_STRUCT(*tracer);

    /* copy the template vtable s*/
    *trace_allocator = s_trace_allocator;
    trace_allocator->impl = tracer;

    s_alloc_tracer_init(tracer, allocator, level, frames_per_stack);
    return trace_allocator;
}

struct aws_allocator *aws_mem_tracer_destroy(struct aws_allocator *trace_allocator) {
    struct alloc_tracer *tracer = trace_allocator->impl;
    struct aws_allocator *allocator = tracer->traced_allocator;

    if (tracer->level != AWS_MEMTRACE_NONE) {
        aws_mutex_lock(&tracer->mutex);
        aws_hash_table_clean_up(&tracer->allocs);
        aws_hash_table_clean_up(&tracer->stacks);
        aws_mutex_unlock(&tracer->mutex);
        aws_mutex_clean_up(&tracer->mutex);
    }

    aws_mem_release(aws_default_allocator(), tracer);
    /* trace_allocator is freed as part of the block tracer was allocated in */

    return allocator;
}

size_t aws_mem_tracer_bytes(struct aws_allocator *trace_allocator) {
    struct alloc_tracer *tracer = trace_allocator->impl;
    if (tracer->level == AWS_MEMTRACE_NONE) {
        return 0;
    }

    return aws_atomic_load_int(&tracer->allocated);
}

size_t aws_mem_tracer_count(struct aws_allocator *trace_allocator) {
    struct alloc_tracer *tracer = trace_allocator->impl;
    if (tracer->level == AWS_MEMTRACE_NONE) {
        return 0;
    }

    aws_mutex_lock(&tracer->mutex);
    size_t count = aws_hash_table_get_entry_count(&tracer->allocs);
    aws_mutex_unlock(&tracer->mutex);
    return count;
}
