/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/**
 * Helper APIs for generating random data from input data stream.
 The producer reads bytes from the end of the input and appends them together
 to generate  a random number in the requested range. If it runs out of input
 data, it will keep returning the same value (min) over and over again.

 */

#ifndef FUZZ_DATA_PRODUCER_H
#define FUZZ_DATA_PRODUCER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "fuzz_helpers.h"

/* Struct used for maintaining the state of the data */
typedef struct FUZZ_dataProducer_s FUZZ_dataProducer_t;

/* Returns a data producer state struct. Use for producer initialization. */
FUZZ_dataProducer_t *FUZZ_dataProducer_create(const uint8_t *data, size_t size);

/* Frees the data producer */
void FUZZ_dataProducer_free(FUZZ_dataProducer_t *producer);

/* Returns value between [min, max] */
uint32_t FUZZ_dataProducer_uint32Range(FUZZ_dataProducer_t *producer, uint32_t min,
                                  uint32_t max);

/* Returns a uint32 value */
uint32_t FUZZ_dataProducer_uint32(FUZZ_dataProducer_t *producer);

/* Returns a signed value between [min, max] */
int32_t FUZZ_dataProducer_int32Range(FUZZ_dataProducer_t *producer,
                                    int32_t min, int32_t max);

/* Returns the size of the remaining bytes of data in the producer */
size_t FUZZ_dataProducer_remainingBytes(FUZZ_dataProducer_t *producer);

/* Rolls back the data producer state to have remainingBytes remaining */
void FUZZ_dataProducer_rollBack(FUZZ_dataProducer_t *producer, size_t remainingBytes);

/* Returns true if the data producer is out of bytes */
int FUZZ_dataProducer_empty(FUZZ_dataProducer_t *producer);

/* Restricts the producer to only the last newSize bytes of data.
If newSize > current data size, nothing happens. Returns the number of bytes
the producer won't use anymore, after contracting. */
size_t FUZZ_dataProducer_contract(FUZZ_dataProducer_t *producer, size_t newSize);

/* Restricts the producer to use only the last X bytes of data, where X is
 a random number in the interval [0, data_size]. Returns the size of the
 remaining data the producer won't use anymore (the prefix). */
size_t FUZZ_dataProducer_reserveDataPrefix(FUZZ_dataProducer_t *producer);
#endif // FUZZ_DATA_PRODUCER_H
