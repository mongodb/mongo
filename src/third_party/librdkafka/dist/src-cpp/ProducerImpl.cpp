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


RdKafka::Producer::~Producer() {
}

static void dr_msg_cb_trampoline(rd_kafka_t *rk,
                                 const rd_kafka_message_t *rkmessage,
                                 void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);
  RdKafka::MessageImpl message(RD_KAFKA_PRODUCER, NULL,
                               (rd_kafka_message_t *)rkmessage, false);
  handle->dr_cb_->dr_cb(message);
}



RdKafka::Producer *RdKafka::Producer::create(const RdKafka::Conf *conf,
                                             std::string &errstr) {
  char errbuf[512];
  const RdKafka::ConfImpl *confimpl =
      dynamic_cast<const RdKafka::ConfImpl *>(conf);
  RdKafka::ProducerImpl *rkp = new RdKafka::ProducerImpl();
  rd_kafka_conf_t *rk_conf   = NULL;

  if (confimpl) {
    if (!confimpl->rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      delete rkp;
      return NULL;
    }

    rkp->set_common_config(confimpl);

    rk_conf = rd_kafka_conf_dup(confimpl->rk_conf_);

    if (confimpl->dr_cb_) {
      rd_kafka_conf_set_dr_msg_cb(rk_conf, dr_msg_cb_trampoline);
      rkp->dr_cb_ = confimpl->dr_cb_;
    }
  }


  rd_kafka_t *rk;
  if (!(rk =
            rd_kafka_new(RD_KAFKA_PRODUCER, rk_conf, errbuf, sizeof(errbuf)))) {
    errstr = errbuf;
    // rd_kafka_new() takes ownership only if succeeds
    if (rk_conf)
      rd_kafka_conf_destroy(rk_conf);
    delete rkp;
    return NULL;
  }

  rkp->rk_ = rk;

  return rkp;
}


RdKafka::ErrorCode RdKafka::ProducerImpl::produce(RdKafka::Topic *topic,
                                                  int32_t partition,
                                                  int msgflags,
                                                  void *payload,
                                                  size_t len,
                                                  const std::string *key,
                                                  void *msg_opaque) {
  RdKafka::TopicImpl *topicimpl = dynamic_cast<RdKafka::TopicImpl *>(topic);

  if (rd_kafka_produce(topicimpl->rkt_, partition, msgflags, payload, len,
                       key ? key->c_str() : NULL, key ? key->size() : 0,
                       msg_opaque) == -1)
    return static_cast<RdKafka::ErrorCode>(rd_kafka_last_error());

  return RdKafka::ERR_NO_ERROR;
}


RdKafka::ErrorCode RdKafka::ProducerImpl::produce(RdKafka::Topic *topic,
                                                  int32_t partition,
                                                  int msgflags,
                                                  void *payload,
                                                  size_t len,
                                                  const void *key,
                                                  size_t key_len,
                                                  void *msg_opaque) {
  RdKafka::TopicImpl *topicimpl = dynamic_cast<RdKafka::TopicImpl *>(topic);

  if (rd_kafka_produce(topicimpl->rkt_, partition, msgflags, payload, len, key,
                       key_len, msg_opaque) == -1)
    return static_cast<RdKafka::ErrorCode>(rd_kafka_last_error());

  return RdKafka::ERR_NO_ERROR;
}


RdKafka::ErrorCode RdKafka::ProducerImpl::produce(
    RdKafka::Topic *topic,
    int32_t partition,
    const std::vector<char> *payload,
    const std::vector<char> *key,
    void *msg_opaque) {
  RdKafka::TopicImpl *topicimpl = dynamic_cast<RdKafka::TopicImpl *>(topic);

  if (rd_kafka_produce(topicimpl->rkt_, partition, RD_KAFKA_MSG_F_COPY,
                       payload ? (void *)&(*payload)[0] : NULL,
                       payload ? payload->size() : 0, key ? &(*key)[0] : NULL,
                       key ? key->size() : 0, msg_opaque) == -1)
    return static_cast<RdKafka::ErrorCode>(rd_kafka_last_error());

  return RdKafka::ERR_NO_ERROR;
}

RdKafka::ErrorCode RdKafka::ProducerImpl::produce(const std::string topic_name,
                                                  int32_t partition,
                                                  int msgflags,
                                                  void *payload,
                                                  size_t len,
                                                  const void *key,
                                                  size_t key_len,
                                                  int64_t timestamp,
                                                  void *msg_opaque) {
  return static_cast<RdKafka::ErrorCode>(rd_kafka_producev(
      rk_, RD_KAFKA_V_TOPIC(topic_name.c_str()),
      RD_KAFKA_V_PARTITION(partition), RD_KAFKA_V_MSGFLAGS(msgflags),
      RD_KAFKA_V_VALUE(payload, len), RD_KAFKA_V_KEY(key, key_len),
      RD_KAFKA_V_TIMESTAMP(timestamp), RD_KAFKA_V_OPAQUE(msg_opaque),
      RD_KAFKA_V_END));
}

RdKafka::ErrorCode RdKafka::ProducerImpl::produce(const std::string topic_name,
                                                  int32_t partition,
                                                  int msgflags,
                                                  void *payload,
                                                  size_t len,
                                                  const void *key,
                                                  size_t key_len,
                                                  int64_t timestamp,
                                                  RdKafka::Headers *headers,
                                                  void *msg_opaque) {
  rd_kafka_headers_t *hdrs          = NULL;
  RdKafka::HeadersImpl *headersimpl = NULL;
  rd_kafka_resp_err_t err;

  if (headers) {
    headersimpl = static_cast<RdKafka::HeadersImpl *>(headers);
    hdrs        = headersimpl->c_ptr();
  }

  err = rd_kafka_producev(
      rk_, RD_KAFKA_V_TOPIC(topic_name.c_str()),
      RD_KAFKA_V_PARTITION(partition), RD_KAFKA_V_MSGFLAGS(msgflags),
      RD_KAFKA_V_VALUE(payload, len), RD_KAFKA_V_KEY(key, key_len),
      RD_KAFKA_V_TIMESTAMP(timestamp), RD_KAFKA_V_OPAQUE(msg_opaque),
      RD_KAFKA_V_HEADERS(hdrs), RD_KAFKA_V_END);

  if (!err && headersimpl) {
    /* A successful producev() call will destroy the C headers. */
    headersimpl->c_headers_destroyed();
    delete headers;
  }

  return static_cast<RdKafka::ErrorCode>(err);
}
