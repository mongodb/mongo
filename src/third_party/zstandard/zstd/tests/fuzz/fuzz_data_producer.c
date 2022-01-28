/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "fuzz_data_producer.h"

struct FUZZ_dataProducer_s{
  const uint8_t *data;
  size_t size;
};

FUZZ_dataProducer_t *FUZZ_dataProducer_create(const uint8_t *data, size_t size) {
    FUZZ_dataProducer_t *producer = FUZZ_malloc(sizeof(FUZZ_dataProducer_t));

    producer->data = data;
    producer->size = size;
    return producer;
}

void FUZZ_dataProducer_free(FUZZ_dataProducer_t *producer) { free(producer); }

uint32_t FUZZ_dataProducer_uint32Range(FUZZ_dataProducer_t *producer, uint32_t min,
                                  uint32_t max) {
    FUZZ_ASSERT(min <= max);

    uint32_t range = max - min;
    uint32_t rolling = range;
    uint32_t result = 0;

    while (rolling > 0 && producer->size > 0) {
      uint8_t next = *(producer->data + producer->size - 1);
      producer->size -= 1;
      result = (result << 8) | next;
      rolling >>= 8;
    }

    if (range == 0xffffffff) {
      return result;
    }

    return min + result % (range + 1);
}

uint32_t FUZZ_dataProducer_uint32(FUZZ_dataProducer_t *producer) {
    return FUZZ_dataProducer_uint32Range(producer, 0, 0xffffffff);
}

int32_t FUZZ_dataProducer_int32Range(FUZZ_dataProducer_t *producer,
                                    int32_t min, int32_t max)
{
    FUZZ_ASSERT(min <= max);

    if (min < 0)
      return (int)FUZZ_dataProducer_uint32Range(producer, 0, max - min) + min;

    return FUZZ_dataProducer_uint32Range(producer, min, max);
}

size_t FUZZ_dataProducer_remainingBytes(FUZZ_dataProducer_t *producer){
    return producer->size;
}

void FUZZ_dataProducer_rollBack(FUZZ_dataProducer_t *producer, size_t remainingBytes)
{
    FUZZ_ASSERT(remainingBytes >= producer->size);
    producer->size = remainingBytes;
}

int FUZZ_dataProducer_empty(FUZZ_dataProducer_t *producer) {
    return producer->size == 0;
}

size_t FUZZ_dataProducer_contract(FUZZ_dataProducer_t *producer, size_t newSize)
{
    newSize = newSize > producer->size ? producer->size : newSize;

    size_t remaining = producer->size - newSize;
    producer->data = producer->data + remaining;
    producer->size = newSize;
    return remaining;
}

size_t FUZZ_dataProducer_reserveDataPrefix(FUZZ_dataProducer_t *producer)
{
    size_t producerSliceSize = FUZZ_dataProducer_uint32Range(
                                  producer, 0, producer->size);
    return FUZZ_dataProducer_contract(producer, producerSliceSize);
}
