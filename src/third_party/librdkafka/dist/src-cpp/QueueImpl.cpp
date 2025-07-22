/*
 * librdkafka - Apache Kafka C/C++ library
 *
 * Copyright (c) 2014-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <cerrno>

#include "rdkafkacpp_int.h"

RdKafka::Queue::~Queue() {
}

RdKafka::Queue *RdKafka::Queue::create(Handle *base) {
  return new RdKafka::QueueImpl(
      rd_kafka_queue_new(dynamic_cast<HandleImpl *>(base)->rk_));
}

RdKafka::ErrorCode RdKafka::QueueImpl::forward(Queue *queue) {
  if (!queue) {
    rd_kafka_queue_forward(queue_, NULL);
  } else {
    QueueImpl *queueimpl = dynamic_cast<QueueImpl *>(queue);
    rd_kafka_queue_forward(queue_, queueimpl->queue_);
  }
  return RdKafka::ERR_NO_ERROR;
}

RdKafka::Message *RdKafka::QueueImpl::consume(int timeout_ms) {
  rd_kafka_message_t *rkmessage;
  rkmessage = rd_kafka_consume_queue(queue_, timeout_ms);

  if (!rkmessage)
    return new RdKafka::MessageImpl(RD_KAFKA_CONSUMER, NULL,
                                    RdKafka::ERR__TIMED_OUT);

  return new RdKafka::MessageImpl(RD_KAFKA_CONSUMER, rkmessage);
}

int RdKafka::QueueImpl::poll(int timeout_ms) {
  return rd_kafka_queue_poll_callback(queue_, timeout_ms);
}

void RdKafka::QueueImpl::io_event_enable(int fd,
                                         const void *payload,
                                         size_t size) {
  rd_kafka_queue_io_event_enable(queue_, fd, payload, size);
}
