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

#include <iostream>
#include <string>
#include <list>
#include <cerrno>

#include "rdkafkacpp_int.h"

RdKafka::Consumer::~Consumer() {
}

RdKafka::Consumer *RdKafka::Consumer::create(const RdKafka::Conf *conf,
                                             std::string &errstr) {
  char errbuf[512];
  const RdKafka::ConfImpl *confimpl =
      dynamic_cast<const RdKafka::ConfImpl *>(conf);
  RdKafka::ConsumerImpl *rkc = new RdKafka::ConsumerImpl();
  rd_kafka_conf_t *rk_conf   = NULL;

  if (confimpl) {
    if (!confimpl->rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      delete rkc;
      return NULL;
    }

    rkc->set_common_config(confimpl);

    rk_conf = rd_kafka_conf_dup(confimpl->rk_conf_);
  }

  rd_kafka_t *rk;
  if (!(rk =
            rd_kafka_new(RD_KAFKA_CONSUMER, rk_conf, errbuf, sizeof(errbuf)))) {
    errstr = errbuf;
    // rd_kafka_new() takes ownership only if succeeds
    if (rk_conf)
      rd_kafka_conf_destroy(rk_conf);
    delete rkc;
    return NULL;
  }

  rkc->rk_ = rk;


  return rkc;
}

int64_t RdKafka::Consumer::OffsetTail(int64_t offset) {
  return RD_KAFKA_OFFSET_TAIL(offset);
}

RdKafka::ErrorCode RdKafka::ConsumerImpl::start(Topic *topic,
                                                int32_t partition,
                                                int64_t offset) {
  RdKafka::TopicImpl *topicimpl = dynamic_cast<RdKafka::TopicImpl *>(topic);

  if (rd_kafka_consume_start(topicimpl->rkt_, partition, offset) == -1)
    return static_cast<RdKafka::ErrorCode>(rd_kafka_last_error());

  return RdKafka::ERR_NO_ERROR;
}


RdKafka::ErrorCode RdKafka::ConsumerImpl::start(Topic *topic,
                                                int32_t partition,
                                                int64_t offset,
                                                Queue *queue) {
  RdKafka::TopicImpl *topicimpl = dynamic_cast<RdKafka::TopicImpl *>(topic);
  RdKafka::QueueImpl *queueimpl = dynamic_cast<RdKafka::QueueImpl *>(queue);

  if (rd_kafka_consume_start_queue(topicimpl->rkt_, partition, offset,
                                   queueimpl->queue_) == -1)
    return static_cast<RdKafka::ErrorCode>(rd_kafka_last_error());

  return RdKafka::ERR_NO_ERROR;
}


RdKafka::ErrorCode RdKafka::ConsumerImpl::stop(Topic *topic,
                                               int32_t partition) {
  RdKafka::TopicImpl *topicimpl = dynamic_cast<RdKafka::TopicImpl *>(topic);

  if (rd_kafka_consume_stop(topicimpl->rkt_, partition) == -1)
    return static_cast<RdKafka::ErrorCode>(rd_kafka_last_error());

  return RdKafka::ERR_NO_ERROR;
}

RdKafka::ErrorCode RdKafka::ConsumerImpl::seek(Topic *topic,
                                               int32_t partition,
                                               int64_t offset,
                                               int timeout_ms) {
  RdKafka::TopicImpl *topicimpl = dynamic_cast<RdKafka::TopicImpl *>(topic);

  if (rd_kafka_seek(topicimpl->rkt_, partition, offset, timeout_ms) == -1)
    return static_cast<RdKafka::ErrorCode>(rd_kafka_last_error());

  return RdKafka::ERR_NO_ERROR;
}

RdKafka::Message *RdKafka::ConsumerImpl::consume(Topic *topic,
                                                 int32_t partition,
                                                 int timeout_ms) {
  RdKafka::TopicImpl *topicimpl = dynamic_cast<RdKafka::TopicImpl *>(topic);
  rd_kafka_message_t *rkmessage;

  rkmessage = rd_kafka_consume(topicimpl->rkt_, partition, timeout_ms);
  if (!rkmessage)
    return new RdKafka::MessageImpl(
        RD_KAFKA_CONSUMER, topic,
        static_cast<RdKafka::ErrorCode>(rd_kafka_last_error()));

  return new RdKafka::MessageImpl(RD_KAFKA_CONSUMER, topic, rkmessage);
}

namespace {
/* Helper struct for `consume_callback'.
 * Encapsulates the values we need in order to call `rd_kafka_consume_callback'
 * and keep track of the C++ callback function and `opaque' value.
 */
struct ConsumerImplCallback {
  ConsumerImplCallback(RdKafka::Topic *topic,
                       RdKafka::ConsumeCb *cb,
                       void *data) :
      topic(topic), cb_cls(cb), cb_data(data) {
  }
  /* This function is the one we give to `rd_kafka_consume_callback', with
   * the `opaque' pointer pointing to an instance of this struct, in which
   * we can find the C++ callback and `cb_data'.
   */
  static void consume_cb_trampoline(rd_kafka_message_t *msg, void *opaque) {
    ConsumerImplCallback *instance =
        static_cast<ConsumerImplCallback *>(opaque);
    RdKafka::MessageImpl message(RD_KAFKA_CONSUMER, instance->topic, msg,
                                 false /*don't free*/);
    instance->cb_cls->consume_cb(message, instance->cb_data);
  }
  RdKafka::Topic *topic;
  RdKafka::ConsumeCb *cb_cls;
  void *cb_data;
};
}  // namespace

int RdKafka::ConsumerImpl::consume_callback(RdKafka::Topic *topic,
                                            int32_t partition,
                                            int timeout_ms,
                                            RdKafka::ConsumeCb *consume_cb,
                                            void *opaque) {
  RdKafka::TopicImpl *topicimpl = static_cast<RdKafka::TopicImpl *>(topic);
  ConsumerImplCallback context(topic, consume_cb, opaque);
  return rd_kafka_consume_callback(topicimpl->rkt_, partition, timeout_ms,
                                   &ConsumerImplCallback::consume_cb_trampoline,
                                   &context);
}


RdKafka::Message *RdKafka::ConsumerImpl::consume(Queue *queue, int timeout_ms) {
  RdKafka::QueueImpl *queueimpl = dynamic_cast<RdKafka::QueueImpl *>(queue);
  rd_kafka_message_t *rkmessage;

  rkmessage = rd_kafka_consume_queue(queueimpl->queue_, timeout_ms);
  if (!rkmessage)
    return new RdKafka::MessageImpl(
        RD_KAFKA_CONSUMER, NULL,
        static_cast<RdKafka::ErrorCode>(rd_kafka_last_error()));
  /*
   * Recover our Topic * from the topic conf's opaque field, which we
   * set in RdKafka::Topic::create() for just this kind of situation.
   */
  void *opaque = rd_kafka_topic_opaque(rkmessage->rkt);
  Topic *topic = static_cast<Topic *>(opaque);

  return new RdKafka::MessageImpl(RD_KAFKA_CONSUMER, topic, rkmessage);
}

namespace {
/* Helper struct for `consume_callback' with a Queue.
 * Encapsulates the values we need in order to call `rd_kafka_consume_callback'
 * and keep track of the C++ callback function and `opaque' value.
 */
struct ConsumerImplQueueCallback {
  ConsumerImplQueueCallback(RdKafka::ConsumeCb *cb, void *data) :
      cb_cls(cb), cb_data(data) {
  }
  /* This function is the one we give to `rd_kafka_consume_callback', with
   * the `opaque' pointer pointing to an instance of this struct, in which
   * we can find the C++ callback and `cb_data'.
   */
  static void consume_cb_trampoline(rd_kafka_message_t *msg, void *opaque) {
    ConsumerImplQueueCallback *instance =
        static_cast<ConsumerImplQueueCallback *>(opaque);
    /*
     * Recover our Topic * from the topic conf's opaque field, which we
     * set in RdKafka::Topic::create() for just this kind of situation.
     */
    void *topic_opaque    = rd_kafka_topic_opaque(msg->rkt);
    RdKafka::Topic *topic = static_cast<RdKafka::Topic *>(topic_opaque);
    RdKafka::MessageImpl message(RD_KAFKA_CONSUMER, topic, msg,
                                 false /*don't free*/);
    instance->cb_cls->consume_cb(message, instance->cb_data);
  }
  RdKafka::ConsumeCb *cb_cls;
  void *cb_data;
};
}  // namespace

int RdKafka::ConsumerImpl::consume_callback(Queue *queue,
                                            int timeout_ms,
                                            RdKafka::ConsumeCb *consume_cb,
                                            void *opaque) {
  RdKafka::QueueImpl *queueimpl = dynamic_cast<RdKafka::QueueImpl *>(queue);
  ConsumerImplQueueCallback context(consume_cb, opaque);
  return rd_kafka_consume_callback_queue(
      queueimpl->queue_, timeout_ms,
      &ConsumerImplQueueCallback::consume_cb_trampoline, &context);
}
