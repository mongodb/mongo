/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#if defined(_C11_ATOMICS)
#include "api_c11_atomics.h"
#elif defined(_C11_ACQ_REL_BARRIERS)
#include "api_c11_acq_rel_barriers.h"
#elif defined(_C11_FULL_BARRIERS)
#include "api_c11_full_barriers.h"
#elif defined(_GCC_ATOMICS)
#include "api_gcc_atomics.h"
#elif defined(_GCC_ACQ_REL_BARRIERS)
#include "api_gcc_acq_rel_barriers.h"
#elif defined(_GCC_FULL_BARRIERS)
#include "api_gcc_full_barriers.h"
#elif defined(_WT_STORE_LOAD_WITH_BARRIERS)
#include "api_wt_store_load_with_barriers.h"
#elif defined(_WT_ACQ_REL_BARRIERS)
#include "api_wt_acq_rel_barriers.h"
#elif defined(_WT_FULL_BARRIERS)
#include "api_wt_full_barriers.h"
#elif defined(_WT_ATOMICS)
#include "api_wt_atomics.h"
#elif defined(_DUMMY_ATOMICS)
#include "api_dummy.h"
#else
#include "api_dummy.h" // For VSCode to parse APIs
#error "Unknown Mode"
#endif

// Shared variables
#define MESSAGE_SIZE 100
typedef char message_t[MESSAGE_SIZE];
static message_t message;
static uint64_t pos;
ATOMIC_DEFINE(count, 1);

// Service variables
static const uint64_t num_iters = 100;
#define NUM_THREADS 4
static uint64_t thread_ids[NUM_THREADS];
static pthread_t thread_pool[NUM_THREADS];

void *ping_pong(void *arg);

void *ping_pong(void *arg) {
    uint64_t *thread_id = (uint64_t *)arg;
    value_t value;

    do {
        do {
            value = atomic_load_acquire(&count);
        } while(value % NUM_THREADS != *thread_id);

        message[pos % MESSAGE_SIZE] = 'a' + ((char)(pos / MESSAGE_SIZE) % (255 - 'a'));
        ++pos;
        printf("%lu: %s\n", pos % MESSAGE_SIZE, message);

        atomic_store_release(&count, value + 1);
    } while (value < num_iters);

    return NULL;
}

int main(void) {
    printf("Implementation: %s\n", get_mode());

    memset(message, '!', sizeof(message));
    pos = 0;

    for (uint64_t i = 0; i < NUM_THREADS; ++i) {
        thread_ids[i] = i;
        pthread_create(&thread_pool[i], NULL, ping_pong, &thread_ids[i]);
    }

    for (uint64_t i = 0; i < NUM_THREADS; ++i)
        pthread_join(thread_pool[i], NULL);

    return 0;
}
