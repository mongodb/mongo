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

#include "rdkafkacpp_int.h"

void RdKafka::consume_cb_trampoline(rd_kafka_message_t *msg, void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);
  RdKafka::Topic *topic = static_cast<Topic *>(rd_kafka_topic_opaque(msg->rkt));

  RdKafka::MessageImpl message(RD_KAFKA_CONSUMER, topic, msg,
                               false /*don't free*/);

  handle->consume_cb_->consume_cb(message, opaque);
}

void RdKafka::log_cb_trampoline(const rd_kafka_t *rk,
                                int level,
                                const char *fac,
                                const char *buf) {
  if (!rk) {
    rd_kafka_log_print(rk, level, fac, buf);
    return;
  }

  void *opaque                = rd_kafka_opaque(rk);
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);

  if (!handle->event_cb_) {
    rd_kafka_log_print(rk, level, fac, buf);
    return;
  }

  RdKafka::EventImpl event(RdKafka::Event::EVENT_LOG, RdKafka::ERR_NO_ERROR,
                           static_cast<RdKafka::Event::Severity>(level), fac,
                           buf);

  handle->event_cb_->event_cb(event);
}


void RdKafka::error_cb_trampoline(rd_kafka_t *rk,
                                  int err,
                                  const char *reason,
                                  void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);
  char errstr[512];
  bool is_fatal = false;

  if (err == RD_KAFKA_RESP_ERR__FATAL) {
    /* Translate to underlying fatal error code and string */
    err = rd_kafka_fatal_error(rk, errstr, sizeof(errstr));
    if (err)
      reason = errstr;
    is_fatal = true;
  }
  RdKafka::EventImpl event(RdKafka::Event::EVENT_ERROR,
                           static_cast<RdKafka::ErrorCode>(err),
                           RdKafka::Event::EVENT_SEVERITY_ERROR, NULL, reason);
  event.fatal_ = is_fatal;
  handle->event_cb_->event_cb(event);
}


void RdKafka::throttle_cb_trampoline(rd_kafka_t *rk,
                                     const char *broker_name,
                                     int32_t broker_id,
                                     int throttle_time_ms,
                                     void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);

  RdKafka::EventImpl event(RdKafka::Event::EVENT_THROTTLE);
  event.str_           = broker_name;
  event.id_            = broker_id;
  event.throttle_time_ = throttle_time_ms;

  handle->event_cb_->event_cb(event);
}


int RdKafka::stats_cb_trampoline(rd_kafka_t *rk,
                                 char *json,
                                 size_t json_len,
                                 void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);

  RdKafka::EventImpl event(RdKafka::Event::EVENT_STATS, RdKafka::ERR_NO_ERROR,
                           RdKafka::Event::EVENT_SEVERITY_INFO, NULL, json);

  handle->event_cb_->event_cb(event);

  return 0;
}


int RdKafka::socket_cb_trampoline(int domain,
                                  int type,
                                  int protocol,
                                  void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);

  return handle->socket_cb_->socket_cb(domain, type, protocol);
}

int RdKafka::open_cb_trampoline(const char *pathname,
                                int flags,
                                mode_t mode,
                                void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);

  return handle->open_cb_->open_cb(pathname, flags, static_cast<int>(mode));
}

void RdKafka::oauthbearer_token_refresh_cb_trampoline(
    rd_kafka_t *rk,
    const char *oauthbearer_config,
    void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);

  handle->oauthbearer_token_refresh_cb_->oauthbearer_token_refresh_cb(
      handle, std::string(oauthbearer_config ? oauthbearer_config : ""));
}


int RdKafka::ssl_cert_verify_cb_trampoline(rd_kafka_t *rk,
                                           const char *broker_name,
                                           int32_t broker_id,
                                           int *x509_error,
                                           int depth,
                                           const char *buf,
                                           size_t size,
                                           char *errstr,
                                           size_t errstr_size,
                                           void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);
  std::string errbuf;

  bool res = 0 != handle->ssl_cert_verify_cb_->ssl_cert_verify_cb(
                      std::string(broker_name), broker_id, x509_error, depth,
                      buf, size, errbuf);

  if (res)
    return (int)res;

  size_t errlen =
      errbuf.size() > errstr_size - 1 ? errstr_size - 1 : errbuf.size();

  memcpy(errstr, errbuf.c_str(), errlen);
  if (errstr_size > 0)
    errstr[errlen] = '\0';

  return (int)res;
}


RdKafka::ErrorCode RdKafka::HandleImpl::metadata(bool all_topics,
                                                 const Topic *only_rkt,
                                                 Metadata **metadatap,
                                                 int timeout_ms) {
  const rd_kafka_metadata_t *cmetadatap = NULL;

  rd_kafka_topic_t *topic =
      only_rkt ? static_cast<const TopicImpl *>(only_rkt)->rkt_ : NULL;

  const rd_kafka_resp_err_t rc =
      rd_kafka_metadata(rk_, all_topics, topic, &cmetadatap, timeout_ms);

  *metadatap = (rc == RD_KAFKA_RESP_ERR_NO_ERROR)
                   ? new RdKafka::MetadataImpl(cmetadatap)
                   : NULL;

  return static_cast<RdKafka::ErrorCode>(rc);
}

/**
 * Convert a list of C partitions to C++ partitions
 */
static void c_parts_to_partitions(
    const rd_kafka_topic_partition_list_t *c_parts,
    std::vector<RdKafka::TopicPartition *> &partitions) {
  partitions.resize(c_parts->cnt);
  for (int i = 0; i < c_parts->cnt; i++)
    partitions[i] = new RdKafka::TopicPartitionImpl(&c_parts->elems[i]);
}

static void free_partition_vector(std::vector<RdKafka::TopicPartition *> &v) {
  for (unsigned int i = 0; i < v.size(); i++)
    delete v[i];
  v.clear();
}

void RdKafka::rebalance_cb_trampoline(
    rd_kafka_t *rk,
    rd_kafka_resp_err_t err,
    rd_kafka_topic_partition_list_t *c_partitions,
    void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);
  std::vector<RdKafka::TopicPartition *> partitions;

  c_parts_to_partitions(c_partitions, partitions);

  handle->rebalance_cb_->rebalance_cb(
      dynamic_cast<RdKafka::KafkaConsumer *>(handle),
      static_cast<RdKafka::ErrorCode>(err), partitions);

  free_partition_vector(partitions);
}


void RdKafka::offset_commit_cb_trampoline0(
    rd_kafka_t *rk,
    rd_kafka_resp_err_t err,
    rd_kafka_topic_partition_list_t *c_offsets,
    void *opaque) {
  OffsetCommitCb *cb = static_cast<RdKafka::OffsetCommitCb *>(opaque);
  std::vector<RdKafka::TopicPartition *> offsets;

  if (c_offsets)
    c_parts_to_partitions(c_offsets, offsets);

  cb->offset_commit_cb(static_cast<RdKafka::ErrorCode>(err), offsets);

  free_partition_vector(offsets);
}

static void offset_commit_cb_trampoline(
    rd_kafka_t *rk,
    rd_kafka_resp_err_t err,
    rd_kafka_topic_partition_list_t *c_offsets,
    void *opaque) {
  RdKafka::HandleImpl *handle = static_cast<RdKafka::HandleImpl *>(opaque);
  RdKafka::offset_commit_cb_trampoline0(rk, err, c_offsets,
                                        handle->offset_commit_cb_);
}


void RdKafka::HandleImpl::set_common_config(const RdKafka::ConfImpl *confimpl) {
  rd_kafka_conf_set_opaque(confimpl->rk_conf_, this);

  if (confimpl->event_cb_) {
    rd_kafka_conf_set_log_cb(confimpl->rk_conf_, RdKafka::log_cb_trampoline);
    rd_kafka_conf_set_error_cb(confimpl->rk_conf_,
                               RdKafka::error_cb_trampoline);
    rd_kafka_conf_set_throttle_cb(confimpl->rk_conf_,
                                  RdKafka::throttle_cb_trampoline);
    rd_kafka_conf_set_stats_cb(confimpl->rk_conf_,
                               RdKafka::stats_cb_trampoline);
    event_cb_ = confimpl->event_cb_;
  }

  if (confimpl->oauthbearer_token_refresh_cb_) {
    rd_kafka_conf_set_oauthbearer_token_refresh_cb(
        confimpl->rk_conf_, RdKafka::oauthbearer_token_refresh_cb_trampoline);
    oauthbearer_token_refresh_cb_ = confimpl->oauthbearer_token_refresh_cb_;
  }

  if (confimpl->socket_cb_) {
    rd_kafka_conf_set_socket_cb(confimpl->rk_conf_,
                                RdKafka::socket_cb_trampoline);
    socket_cb_ = confimpl->socket_cb_;
  }

  if (confimpl->ssl_cert_verify_cb_) {
    rd_kafka_conf_set_ssl_cert_verify_cb(
        confimpl->rk_conf_, RdKafka::ssl_cert_verify_cb_trampoline);
    ssl_cert_verify_cb_ = confimpl->ssl_cert_verify_cb_;
  }

  if (confimpl->open_cb_) {
#ifndef _WIN32
    rd_kafka_conf_set_open_cb(confimpl->rk_conf_, RdKafka::open_cb_trampoline);
    open_cb_ = confimpl->open_cb_;
#endif
  }

  if (confimpl->rebalance_cb_) {
    rd_kafka_conf_set_rebalance_cb(confimpl->rk_conf_,
                                   RdKafka::rebalance_cb_trampoline);
    rebalance_cb_ = confimpl->rebalance_cb_;
  }

  if (confimpl->offset_commit_cb_) {
    rd_kafka_conf_set_offset_commit_cb(confimpl->rk_conf_,
                                       offset_commit_cb_trampoline);
    offset_commit_cb_ = confimpl->offset_commit_cb_;
  }

  if (confimpl->consume_cb_) {
    rd_kafka_conf_set_consume_cb(confimpl->rk_conf_,
                                 RdKafka::consume_cb_trampoline);
    consume_cb_ = confimpl->consume_cb_;
  }
}


RdKafka::ErrorCode RdKafka::HandleImpl::pause(
    std::vector<RdKafka::TopicPartition *> &partitions) {
  rd_kafka_topic_partition_list_t *c_parts;
  rd_kafka_resp_err_t err;

  c_parts = partitions_to_c_parts(partitions);

  err = rd_kafka_pause_partitions(rk_, c_parts);

  if (!err)
    update_partitions_from_c_parts(partitions, c_parts);

  rd_kafka_topic_partition_list_destroy(c_parts);

  return static_cast<RdKafka::ErrorCode>(err);
}


RdKafka::ErrorCode RdKafka::HandleImpl::resume(
    std::vector<RdKafka::TopicPartition *> &partitions) {
  rd_kafka_topic_partition_list_t *c_parts;
  rd_kafka_resp_err_t err;

  c_parts = partitions_to_c_parts(partitions);

  err = rd_kafka_resume_partitions(rk_, c_parts);

  if (!err)
    update_partitions_from_c_parts(partitions, c_parts);

  rd_kafka_topic_partition_list_destroy(c_parts);

  return static_cast<RdKafka::ErrorCode>(err);
}

RdKafka::Queue *RdKafka::HandleImpl::get_partition_queue(
    const TopicPartition *part) {
  rd_kafka_queue_t *rkqu;
  rkqu = rd_kafka_queue_get_partition(rk_, part->topic().c_str(),
                                      part->partition());

  if (rkqu == NULL)
    return NULL;

  return new QueueImpl(rkqu);
}

RdKafka::ErrorCode RdKafka::HandleImpl::set_log_queue(RdKafka::Queue *queue) {
  rd_kafka_queue_t *rkqu = NULL;
  if (queue) {
    QueueImpl *queueimpl = dynamic_cast<QueueImpl *>(queue);
    rkqu                 = queueimpl->queue_;
  }
  return static_cast<RdKafka::ErrorCode>(rd_kafka_set_log_queue(rk_, rkqu));
}

namespace RdKafka {

rd_kafka_topic_partition_list_t *partitions_to_c_parts(
    const std::vector<RdKafka::TopicPartition *> &partitions) {
  rd_kafka_topic_partition_list_t *c_parts;

  c_parts = rd_kafka_topic_partition_list_new((int)partitions.size());

  for (unsigned int i = 0; i < partitions.size(); i++) {
    const RdKafka::TopicPartitionImpl *tpi =
        dynamic_cast<const RdKafka::TopicPartitionImpl *>(partitions[i]);
    rd_kafka_topic_partition_t *rktpar = rd_kafka_topic_partition_list_add(
        c_parts, tpi->topic_.c_str(), tpi->partition_);
    rktpar->offset = tpi->offset_;
  }

  return c_parts;
}


/**
 * @brief Update the application provided 'partitions' with info from 'c_parts'
 */
void update_partitions_from_c_parts(
    std::vector<RdKafka::TopicPartition *> &partitions,
    const rd_kafka_topic_partition_list_t *c_parts) {
  for (int i = 0; i < c_parts->cnt; i++) {
    rd_kafka_topic_partition_t *p = &c_parts->elems[i];

    /* Find corresponding C++ entry */
    for (unsigned int j = 0; j < partitions.size(); j++) {
      RdKafka::TopicPartitionImpl *pp =
          dynamic_cast<RdKafka::TopicPartitionImpl *>(partitions[j]);
      if (!strcmp(p->topic, pp->topic_.c_str()) &&
          p->partition == pp->partition_) {
        pp->offset_ = p->offset;
        pp->err_    = static_cast<RdKafka::ErrorCode>(p->err);
      }
    }
  }
}

}  // namespace RdKafka
