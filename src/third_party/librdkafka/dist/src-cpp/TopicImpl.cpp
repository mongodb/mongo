/*
 * librdkafka - Apache Kafka C/C++ library
 *
 * Copyright (c) 2014 Magnus Edenhill
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

const int32_t RdKafka::Topic::PARTITION_UA = RD_KAFKA_PARTITION_UA;

const int64_t RdKafka::Topic::OFFSET_BEGINNING = RD_KAFKA_OFFSET_BEGINNING;

const int64_t RdKafka::Topic::OFFSET_END = RD_KAFKA_OFFSET_END;

const int64_t RdKafka::Topic::OFFSET_STORED = RD_KAFKA_OFFSET_STORED;

const int64_t RdKafka::Topic::OFFSET_INVALID = RD_KAFKA_OFFSET_INVALID;

RdKafka::Topic::~Topic() {
}

static int32_t partitioner_cb_trampoline(const rd_kafka_topic_t *rkt,
                                         const void *keydata,
                                         size_t keylen,
                                         int32_t partition_cnt,
                                         void *rkt_opaque,
                                         void *msg_opaque) {
  RdKafka::TopicImpl *topicimpl = static_cast<RdKafka::TopicImpl *>(rkt_opaque);
  std::string key(static_cast<const char *>(keydata), keylen);
  return topicimpl->partitioner_cb_->partitioner_cb(topicimpl, &key,
                                                    partition_cnt, msg_opaque);
}

static int32_t partitioner_kp_cb_trampoline(const rd_kafka_topic_t *rkt,
                                            const void *keydata,
                                            size_t keylen,
                                            int32_t partition_cnt,
                                            void *rkt_opaque,
                                            void *msg_opaque) {
  RdKafka::TopicImpl *topicimpl = static_cast<RdKafka::TopicImpl *>(rkt_opaque);
  return topicimpl->partitioner_kp_cb_->partitioner_cb(
      topicimpl, keydata, keylen, partition_cnt, msg_opaque);
}



RdKafka::Topic *RdKafka::Topic::create(Handle *base,
                                       const std::string &topic_str,
                                       const Conf *conf,
                                       std::string &errstr) {
  const RdKafka::ConfImpl *confimpl =
      static_cast<const RdKafka::ConfImpl *>(conf);
  rd_kafka_topic_t *rkt;
  rd_kafka_topic_conf_t *rkt_conf;
  rd_kafka_t *rk = dynamic_cast<HandleImpl *>(base)->rk_;

  RdKafka::TopicImpl *topic = new RdKafka::TopicImpl();

  if (!confimpl) {
    /* Reuse default topic config, but we need our own copy to
     * set the topic opaque. */
    rkt_conf = rd_kafka_default_topic_conf_dup(rk);
  } else {
    /* Make a copy of conf struct to allow Conf reuse. */
    rkt_conf = rd_kafka_topic_conf_dup(confimpl->rkt_conf_);
  }

  /* Set topic opaque to the topic so that we can reach our topic object
   * from whatever callbacks get registered.
   * The application itself will not need these opaques since their
   * callbacks are class based. */
  rd_kafka_topic_conf_set_opaque(rkt_conf, static_cast<void *>(topic));

  if (confimpl) {
    if (confimpl->partitioner_cb_) {
      rd_kafka_topic_conf_set_partitioner_cb(rkt_conf,
                                             partitioner_cb_trampoline);
      topic->partitioner_cb_ = confimpl->partitioner_cb_;
    } else if (confimpl->partitioner_kp_cb_) {
      rd_kafka_topic_conf_set_partitioner_cb(rkt_conf,
                                             partitioner_kp_cb_trampoline);
      topic->partitioner_kp_cb_ = confimpl->partitioner_kp_cb_;
    }
  }


  if (!(rkt = rd_kafka_topic_new(rk, topic_str.c_str(), rkt_conf))) {
    errstr = rd_kafka_err2str(rd_kafka_last_error());
    delete topic;
    rd_kafka_topic_conf_destroy(rkt_conf);
    return NULL;
  }

  topic->rkt_ = rkt;

  return topic;
}
