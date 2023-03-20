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

#ifndef _RDKAFKACPP_INT_H_
#define _RDKAFKACPP_INT_H_

#include <string>
#include <iostream>
#include <cstring>
#include <stdlib.h>

#include "rdkafkacpp.h"

extern "C" {
#include "../src/rdkafka.h"
}

#ifdef _WIN32
/* Visual Studio */
#include "../src/win32_config.h"
#else
/* POSIX / UNIX based systems */
#include "../config.h" /* mklove output */
#endif

#ifdef _MSC_VER
typedef int mode_t;
#pragma warning(disable : 4250)
#endif


namespace RdKafka {

void consume_cb_trampoline(rd_kafka_message_t *msg, void *opaque);
void log_cb_trampoline(const rd_kafka_t *rk,
                       int level,
                       const char *fac,
                       const char *buf);
void error_cb_trampoline(rd_kafka_t *rk,
                         int err,
                         const char *reason,
                         void *opaque);
void throttle_cb_trampoline(rd_kafka_t *rk,
                            const char *broker_name,
                            int32_t broker_id,
                            int throttle_time_ms,
                            void *opaque);
int stats_cb_trampoline(rd_kafka_t *rk,
                        char *json,
                        size_t json_len,
                        void *opaque);
int socket_cb_trampoline(int domain, int type, int protocol, void *opaque);
int open_cb_trampoline(const char *pathname,
                       int flags,
                       mode_t mode,
                       void *opaque);
void rebalance_cb_trampoline(rd_kafka_t *rk,
                             rd_kafka_resp_err_t err,
                             rd_kafka_topic_partition_list_t *c_partitions,
                             void *opaque);
void offset_commit_cb_trampoline0(rd_kafka_t *rk,
                                  rd_kafka_resp_err_t err,
                                  rd_kafka_topic_partition_list_t *c_offsets,
                                  void *opaque);
void oauthbearer_token_refresh_cb_trampoline(rd_kafka_t *rk,
                                             const char *oauthbearer_config,
                                             void *opaque);

int ssl_cert_verify_cb_trampoline(rd_kafka_t *rk,
                                  const char *broker_name,
                                  int32_t broker_id,
                                  int *x509_error,
                                  int depth,
                                  const char *buf,
                                  size_t size,
                                  char *errstr,
                                  size_t errstr_size,
                                  void *opaque);

rd_kafka_topic_partition_list_t *partitions_to_c_parts(
    const std::vector<TopicPartition *> &partitions);

/**
 * @brief Update the application provided 'partitions' with info from 'c_parts'
 */
void update_partitions_from_c_parts(
    std::vector<TopicPartition *> &partitions,
    const rd_kafka_topic_partition_list_t *c_parts);


class ErrorImpl : public Error {
 public:
  ~ErrorImpl() {
    rd_kafka_error_destroy(c_error_);
  }

  ErrorImpl(ErrorCode code, const std::string *errstr) {
    c_error_ = rd_kafka_error_new(static_cast<rd_kafka_resp_err_t>(code),
                                  errstr ? "%s" : NULL,
                                  errstr ? errstr->c_str() : NULL);
  }

  ErrorImpl(rd_kafka_error_t *c_error) : c_error_(c_error) {
  }

  static Error *create(ErrorCode code, const std::string *errstr) {
    return new ErrorImpl(code, errstr);
  }

  ErrorCode code() const {
    return static_cast<ErrorCode>(rd_kafka_error_code(c_error_));
  }

  std::string name() const {
    return std::string(rd_kafka_error_name(c_error_));
  }

  std::string str() const {
    return std::string(rd_kafka_error_string(c_error_));
  }

  bool is_fatal() const {
    return !!rd_kafka_error_is_fatal(c_error_);
  }

  bool is_retriable() const {
    return !!rd_kafka_error_is_retriable(c_error_);
  }

  bool txn_requires_abort() const {
    return !!rd_kafka_error_txn_requires_abort(c_error_);
  }

  rd_kafka_error_t *c_error_;
};


class EventImpl : public Event {
 public:
  ~EventImpl() {
  }

  EventImpl(Type type,
            ErrorCode err,
            Severity severity,
            const char *fac,
            const char *str) :
      type_(type),
      err_(err),
      severity_(severity),
      fac_(fac ? fac : ""),
      str_(str),
      id_(0),
      throttle_time_(0),
      fatal_(false) {
  }

  EventImpl(Type type) :
      type_(type),
      err_(ERR_NO_ERROR),
      severity_(EVENT_SEVERITY_EMERG),
      fac_(""),
      str_(""),
      id_(0),
      throttle_time_(0),
      fatal_(false) {
  }

  Type type() const {
    return type_;
  }
  ErrorCode err() const {
    return err_;
  }
  Severity severity() const {
    return severity_;
  }
  std::string fac() const {
    return fac_;
  }
  std::string str() const {
    return str_;
  }
  std::string broker_name() const {
    if (type_ == EVENT_THROTTLE)
      return str_;
    else
      return std::string("");
  }
  int broker_id() const {
    return id_;
  }
  int throttle_time() const {
    return throttle_time_;
  }

  bool fatal() const {
    return fatal_;
  }

  Type type_;
  ErrorCode err_;
  Severity severity_;
  std::string fac_;
  std::string str_; /* reused for THROTTLE broker_name */
  int id_;
  int throttle_time_;
  bool fatal_;
};

class QueueImpl : virtual public Queue {
 public:
  QueueImpl(rd_kafka_queue_t *c_rkqu) : queue_(c_rkqu) {
  }
  ~QueueImpl() {
    rd_kafka_queue_destroy(queue_);
  }
  static Queue *create(Handle *base);
  ErrorCode forward(Queue *queue);
  Message *consume(int timeout_ms);
  int poll(int timeout_ms);
  void io_event_enable(int fd, const void *payload, size_t size);

  rd_kafka_queue_t *queue_;
};



class HeadersImpl : public Headers {
 public:
  HeadersImpl() : headers_(rd_kafka_headers_new(8)) {
  }

  HeadersImpl(rd_kafka_headers_t *headers) : headers_(headers) {
  }

  HeadersImpl(const std::vector<Header> &headers) {
    if (headers.size() > 0) {
      headers_ = rd_kafka_headers_new(headers.size());
      from_vector(headers);
    } else {
      headers_ = rd_kafka_headers_new(8);
    }
  }

  ~HeadersImpl() {
    if (headers_) {
      rd_kafka_headers_destroy(headers_);
    }
  }

  ErrorCode add(const std::string &key, const char *value) {
    rd_kafka_resp_err_t err;
    err = rd_kafka_header_add(headers_, key.c_str(), key.size(), value, -1);
    return static_cast<RdKafka::ErrorCode>(err);
  }

  ErrorCode add(const std::string &key, const void *value, size_t value_size) {
    rd_kafka_resp_err_t err;
    err = rd_kafka_header_add(headers_, key.c_str(), key.size(), value,
                              value_size);
    return static_cast<RdKafka::ErrorCode>(err);
  }

  ErrorCode add(const std::string &key, const std::string &value) {
    rd_kafka_resp_err_t err;
    err = rd_kafka_header_add(headers_, key.c_str(), key.size(), value.c_str(),
                              value.size());
    return static_cast<RdKafka::ErrorCode>(err);
  }

  ErrorCode add(const Header &header) {
    rd_kafka_resp_err_t err;
    err =
        rd_kafka_header_add(headers_, header.key().c_str(), header.key().size(),
                            header.value(), header.value_size());
    return static_cast<RdKafka::ErrorCode>(err);
  }

  ErrorCode remove(const std::string &key) {
    rd_kafka_resp_err_t err;
    err = rd_kafka_header_remove(headers_, key.c_str());
    return static_cast<RdKafka::ErrorCode>(err);
  }

  std::vector<Headers::Header> get(const std::string &key) const {
    std::vector<Headers::Header> headers;
    const void *value;
    size_t size;
    rd_kafka_resp_err_t err;
    for (size_t idx = 0; !(err = rd_kafka_header_get(headers_, idx, key.c_str(),
                                                     &value, &size));
         idx++) {
      headers.push_back(Headers::Header(key, value, size));
    }
    return headers;
  }

  Headers::Header get_last(const std::string &key) const {
    const void *value;
    size_t size;
    rd_kafka_resp_err_t err;
    err = rd_kafka_header_get_last(headers_, key.c_str(), &value, &size);
    return Headers::Header(key, value, size,
                           static_cast<RdKafka::ErrorCode>(err));
  }

  std::vector<Headers::Header> get_all() const {
    std::vector<Headers::Header> headers;
    size_t idx = 0;
    const char *name;
    const void *valuep;
    size_t size;
    while (!rd_kafka_header_get_all(headers_, idx++, &name, &valuep, &size)) {
      headers.push_back(Headers::Header(name, valuep, size));
    }
    return headers;
  }

  size_t size() const {
    return rd_kafka_header_cnt(headers_);
  }

  /** @brief Reset the C headers pointer to NULL. */
  void c_headers_destroyed() {
    headers_ = NULL;
  }

  /** @returns the underlying C headers, or NULL. */
  rd_kafka_headers_t *c_ptr() {
    return headers_;
  }


 private:
  void from_vector(const std::vector<Header> &headers) {
    if (headers.size() == 0)
      return;
    for (std::vector<Header>::const_iterator it = headers.begin();
         it != headers.end(); it++)
      this->add(*it);
  }

  HeadersImpl(HeadersImpl const &) /*= delete*/;
  HeadersImpl &operator=(HeadersImpl const &) /*= delete*/;

  rd_kafka_headers_t *headers_;
};



class MessageImpl : public Message {
 public:
  ~MessageImpl() {
    if (free_rkmessage_)
      rd_kafka_message_destroy(const_cast<rd_kafka_message_t *>(rkmessage_));
    if (key_)
      delete key_;
    if (headers_)
      delete headers_;
  }

  MessageImpl(rd_kafka_type_t rk_type,
              RdKafka::Topic *topic,
              rd_kafka_message_t *rkmessage) :
      topic_(topic),
      rkmessage_(rkmessage),
      free_rkmessage_(true),
      key_(NULL),
      headers_(NULL),
      rk_type_(rk_type) {
  }

  MessageImpl(rd_kafka_type_t rk_type,
              RdKafka::Topic *topic,
              rd_kafka_message_t *rkmessage,
              bool dofree) :
      topic_(topic),
      rkmessage_(rkmessage),
      free_rkmessage_(dofree),
      key_(NULL),
      headers_(NULL),
      rk_type_(rk_type) {
  }

  MessageImpl(rd_kafka_type_t rk_type, rd_kafka_message_t *rkmessage) :
      topic_(NULL),
      rkmessage_(rkmessage),
      free_rkmessage_(true),
      key_(NULL),
      headers_(NULL),
      rk_type_(rk_type) {
    if (rkmessage->rkt) {
      /* Possibly NULL */
      topic_ = static_cast<Topic *>(rd_kafka_topic_opaque(rkmessage->rkt));
    }
  }

  /* Create errored message */
  MessageImpl(rd_kafka_type_t rk_type,
              RdKafka::Topic *topic,
              RdKafka::ErrorCode err) :
      topic_(topic),
      free_rkmessage_(false),
      key_(NULL),
      headers_(NULL),
      rk_type_(rk_type) {
    rkmessage_ = &rkmessage_err_;
    memset(&rkmessage_err_, 0, sizeof(rkmessage_err_));
    rkmessage_err_.err = static_cast<rd_kafka_resp_err_t>(err);
  }

  std::string errstr() const {
    const char *es;
    /* message_errstr() is only available for the consumer. */
    if (rk_type_ == RD_KAFKA_CONSUMER)
      es = rd_kafka_message_errstr(rkmessage_);
    else
      es = rd_kafka_err2str(rkmessage_->err);

    return std::string(es ? es : "");
  }

  ErrorCode err() const {
    return static_cast<RdKafka::ErrorCode>(rkmessage_->err);
  }

  Topic *topic() const {
    return topic_;
  }
  std::string topic_name() const {
    if (rkmessage_->rkt)
      return rd_kafka_topic_name(rkmessage_->rkt);
    else
      return "";
  }
  int32_t partition() const {
    return rkmessage_->partition;
  }
  void *payload() const {
    return rkmessage_->payload;
  }
  size_t len() const {
    return rkmessage_->len;
  }
  const std::string *key() const {
    if (key_) {
      return key_;
    } else if (rkmessage_->key) {
      key_ = new std::string(static_cast<char const *>(rkmessage_->key),
                             rkmessage_->key_len);
      return key_;
    }
    return NULL;
  }
  const void *key_pointer() const {
    return rkmessage_->key;
  }
  size_t key_len() const {
    return rkmessage_->key_len;
  }

  int64_t offset() const {
    return rkmessage_->offset;
  }

  MessageTimestamp timestamp() const {
    MessageTimestamp ts;
    rd_kafka_timestamp_type_t tstype;
    ts.timestamp = rd_kafka_message_timestamp(rkmessage_, &tstype);
    ts.type      = static_cast<MessageTimestamp::MessageTimestampType>(tstype);
    return ts;
  }

  void *msg_opaque() const {
    return rkmessage_->_private;
  }

  int64_t latency() const {
    return rd_kafka_message_latency(rkmessage_);
  }

  struct rd_kafka_message_s *c_ptr() {
    return rkmessage_;
  }

  Status status() const {
    return static_cast<Status>(rd_kafka_message_status(rkmessage_));
  }

  Headers *headers() {
    ErrorCode err;
    return headers(&err);
  }

  Headers *headers(ErrorCode *err) {
    *err = ERR_NO_ERROR;

    if (!headers_) {
      rd_kafka_headers_t *c_hdrs;
      rd_kafka_resp_err_t c_err;

      if ((c_err = rd_kafka_message_detach_headers(rkmessage_, &c_hdrs))) {
        *err = static_cast<RdKafka::ErrorCode>(c_err);
        return NULL;
      }

      headers_ = new HeadersImpl(c_hdrs);
    }

    return headers_;
  }

  int32_t broker_id() const {
    return rd_kafka_message_broker_id(rkmessage_);
  }


  RdKafka::Topic *topic_;
  rd_kafka_message_t *rkmessage_;
  bool free_rkmessage_;
  /* For error signalling by the C++ layer the .._err_ message is
   * used as a place holder and rkmessage_ is set to point to it. */
  rd_kafka_message_t rkmessage_err_;
  mutable std::string *key_; /* mutable because it's a cached value */

 private:
  /* "delete" copy ctor + copy assignment, for safety of key_ */
  MessageImpl(MessageImpl const &) /*= delete*/;
  MessageImpl &operator=(MessageImpl const &) /*= delete*/;

  RdKafka::Headers *headers_;
  const rd_kafka_type_t rk_type_; /**< Client type */
};


class ConfImpl : public Conf {
 public:
  ConfImpl(ConfType conf_type) :
      consume_cb_(NULL),
      dr_cb_(NULL),
      event_cb_(NULL),
      socket_cb_(NULL),
      open_cb_(NULL),
      partitioner_cb_(NULL),
      partitioner_kp_cb_(NULL),
      rebalance_cb_(NULL),
      offset_commit_cb_(NULL),
      oauthbearer_token_refresh_cb_(NULL),
      ssl_cert_verify_cb_(NULL),
      conf_type_(conf_type),
      rk_conf_(NULL),
      rkt_conf_(NULL) {
  }
  ~ConfImpl() {
    if (rk_conf_)
      rd_kafka_conf_destroy(rk_conf_);
    else if (rkt_conf_)
      rd_kafka_topic_conf_destroy(rkt_conf_);
  }

  Conf::ConfResult set(const std::string &name,
                       const std::string &value,
                       std::string &errstr);

  Conf::ConfResult set(const std::string &name,
                       DeliveryReportCb *dr_cb,
                       std::string &errstr) {
    if (name != "dr_cb") {
      errstr = "Invalid value type, expected RdKafka::DeliveryReportCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    dr_cb_ = dr_cb;
    return Conf::CONF_OK;
  }

  Conf::ConfResult set(const std::string &name,
                       OAuthBearerTokenRefreshCb *oauthbearer_token_refresh_cb,
                       std::string &errstr) {
    if (name != "oauthbearer_token_refresh_cb") {
      errstr =
          "Invalid value type, expected RdKafka::OAuthBearerTokenRefreshCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    oauthbearer_token_refresh_cb_ = oauthbearer_token_refresh_cb;
    return Conf::CONF_OK;
  }

  Conf::ConfResult set(const std::string &name,
                       EventCb *event_cb,
                       std::string &errstr) {
    if (name != "event_cb") {
      errstr = "Invalid value type, expected RdKafka::EventCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    event_cb_ = event_cb;
    return Conf::CONF_OK;
  }

  Conf::ConfResult set(const std::string &name,
                       const Conf *topic_conf,
                       std::string &errstr) {
    const ConfImpl *tconf_impl =
        dynamic_cast<const RdKafka::ConfImpl *>(topic_conf);
    if (name != "default_topic_conf" || !tconf_impl->rkt_conf_) {
      errstr = "Invalid value type, expected RdKafka::Conf";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    rd_kafka_conf_set_default_topic_conf(
        rk_conf_, rd_kafka_topic_conf_dup(tconf_impl->rkt_conf_));

    return Conf::CONF_OK;
  }

  Conf::ConfResult set(const std::string &name,
                       PartitionerCb *partitioner_cb,
                       std::string &errstr) {
    if (name != "partitioner_cb") {
      errstr = "Invalid value type, expected RdKafka::PartitionerCb";
      return Conf::CONF_INVALID;
    }

    if (!rkt_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_TOPIC object";
      return Conf::CONF_INVALID;
    }

    partitioner_cb_ = partitioner_cb;
    return Conf::CONF_OK;
  }

  Conf::ConfResult set(const std::string &name,
                       PartitionerKeyPointerCb *partitioner_kp_cb,
                       std::string &errstr) {
    if (name != "partitioner_key_pointer_cb") {
      errstr = "Invalid value type, expected RdKafka::PartitionerKeyPointerCb";
      return Conf::CONF_INVALID;
    }

    if (!rkt_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_TOPIC object";
      return Conf::CONF_INVALID;
    }

    partitioner_kp_cb_ = partitioner_kp_cb;
    return Conf::CONF_OK;
  }

  Conf::ConfResult set(const std::string &name,
                       SocketCb *socket_cb,
                       std::string &errstr) {
    if (name != "socket_cb") {
      errstr = "Invalid value type, expected RdKafka::SocketCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    socket_cb_ = socket_cb;
    return Conf::CONF_OK;
  }


  Conf::ConfResult set(const std::string &name,
                       OpenCb *open_cb,
                       std::string &errstr) {
    if (name != "open_cb") {
      errstr = "Invalid value type, expected RdKafka::OpenCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    open_cb_ = open_cb;
    return Conf::CONF_OK;
  }



  Conf::ConfResult set(const std::string &name,
                       RebalanceCb *rebalance_cb,
                       std::string &errstr) {
    if (name != "rebalance_cb") {
      errstr = "Invalid value type, expected RdKafka::RebalanceCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    rebalance_cb_ = rebalance_cb;
    return Conf::CONF_OK;
  }


  Conf::ConfResult set(const std::string &name,
                       OffsetCommitCb *offset_commit_cb,
                       std::string &errstr) {
    if (name != "offset_commit_cb") {
      errstr = "Invalid value type, expected RdKafka::OffsetCommitCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    offset_commit_cb_ = offset_commit_cb;
    return Conf::CONF_OK;
  }


  Conf::ConfResult set(const std::string &name,
                       SslCertificateVerifyCb *ssl_cert_verify_cb,
                       std::string &errstr) {
    if (name != "ssl_cert_verify_cb") {
      errstr = "Invalid value type, expected RdKafka::SslCertificateVerifyCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    ssl_cert_verify_cb_ = ssl_cert_verify_cb;
    return Conf::CONF_OK;
  }

  Conf::ConfResult set_engine_callback_data(void *value, std::string &errstr) {
    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    rd_kafka_conf_set_engine_callback_data(rk_conf_, value);
    return Conf::CONF_OK;
  }


  Conf::ConfResult set_ssl_cert(RdKafka::CertificateType cert_type,
                                RdKafka::CertificateEncoding cert_enc,
                                const void *buffer,
                                size_t size,
                                std::string &errstr) {
    rd_kafka_conf_res_t res;
    char errbuf[512];

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    res = rd_kafka_conf_set_ssl_cert(
        rk_conf_, static_cast<rd_kafka_cert_type_t>(cert_type),
        static_cast<rd_kafka_cert_enc_t>(cert_enc), buffer, size, errbuf,
        sizeof(errbuf));

    if (res != RD_KAFKA_CONF_OK)
      errstr = errbuf;

    return static_cast<Conf::ConfResult>(res);
  }

  Conf::ConfResult enable_sasl_queue(bool enable, std::string &errstr) {
    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    rd_kafka_conf_enable_sasl_queue(rk_conf_, enable ? 1 : 0);

    return Conf::CONF_OK;
  }


  Conf::ConfResult get(const std::string &name, std::string &value) const {
    if (name.compare("dr_cb") == 0 || name.compare("event_cb") == 0 ||
        name.compare("partitioner_cb") == 0 ||
        name.compare("partitioner_key_pointer_cb") == 0 ||
        name.compare("socket_cb") == 0 || name.compare("open_cb") == 0 ||
        name.compare("rebalance_cb") == 0 ||
        name.compare("offset_commit_cb") == 0 ||
        name.compare("oauthbearer_token_refresh_cb") == 0 ||
        name.compare("ssl_cert_verify_cb") == 0 ||
        name.compare("set_engine_callback_data") == 0 ||
        name.compare("enable_sasl_queue") == 0) {
      return Conf::CONF_INVALID;
    }
    rd_kafka_conf_res_t res = RD_KAFKA_CONF_INVALID;

    /* Get size of property */
    size_t size;
    if (rk_conf_)
      res = rd_kafka_conf_get(rk_conf_, name.c_str(), NULL, &size);
    else if (rkt_conf_)
      res = rd_kafka_topic_conf_get(rkt_conf_, name.c_str(), NULL, &size);
    if (res != RD_KAFKA_CONF_OK)
      return static_cast<Conf::ConfResult>(res);

    char *tmpValue = new char[size];

    if (rk_conf_)
      res = rd_kafka_conf_get(rk_conf_, name.c_str(), tmpValue, &size);
    else if (rkt_conf_)
      res = rd_kafka_topic_conf_get(rkt_conf_, name.c_str(), tmpValue, &size);

    if (res == RD_KAFKA_CONF_OK)
      value.assign(tmpValue);
    delete[] tmpValue;

    return static_cast<Conf::ConfResult>(res);
  }

  Conf::ConfResult get(DeliveryReportCb *&dr_cb) const {
    if (!rk_conf_)
      return Conf::CONF_INVALID;
    dr_cb = this->dr_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(
      OAuthBearerTokenRefreshCb *&oauthbearer_token_refresh_cb) const {
    if (!rk_conf_)
      return Conf::CONF_INVALID;
    oauthbearer_token_refresh_cb = this->oauthbearer_token_refresh_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(EventCb *&event_cb) const {
    if (!rk_conf_)
      return Conf::CONF_INVALID;
    event_cb = this->event_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(PartitionerCb *&partitioner_cb) const {
    if (!rkt_conf_)
      return Conf::CONF_INVALID;
    partitioner_cb = this->partitioner_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(PartitionerKeyPointerCb *&partitioner_kp_cb) const {
    if (!rkt_conf_)
      return Conf::CONF_INVALID;
    partitioner_kp_cb = this->partitioner_kp_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(SocketCb *&socket_cb) const {
    if (!rk_conf_)
      return Conf::CONF_INVALID;
    socket_cb = this->socket_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(OpenCb *&open_cb) const {
    if (!rk_conf_)
      return Conf::CONF_INVALID;
    open_cb = this->open_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(RebalanceCb *&rebalance_cb) const {
    if (!rk_conf_)
      return Conf::CONF_INVALID;
    rebalance_cb = this->rebalance_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(OffsetCommitCb *&offset_commit_cb) const {
    if (!rk_conf_)
      return Conf::CONF_INVALID;
    offset_commit_cb = this->offset_commit_cb_;
    return Conf::CONF_OK;
  }

  Conf::ConfResult get(SslCertificateVerifyCb *&ssl_cert_verify_cb) const {
    if (!rk_conf_)
      return Conf::CONF_INVALID;
    ssl_cert_verify_cb = this->ssl_cert_verify_cb_;
    return Conf::CONF_OK;
  }

  std::list<std::string> *dump();


  Conf::ConfResult set(const std::string &name,
                       ConsumeCb *consume_cb,
                       std::string &errstr) {
    if (name != "consume_cb") {
      errstr = "Invalid value type, expected RdKafka::ConsumeCb";
      return Conf::CONF_INVALID;
    }

    if (!rk_conf_) {
      errstr = "Requires RdKafka::Conf::CONF_GLOBAL object";
      return Conf::CONF_INVALID;
    }

    consume_cb_ = consume_cb;
    return Conf::CONF_OK;
  }

  struct rd_kafka_conf_s *c_ptr_global() {
    if (conf_type_ == CONF_GLOBAL)
      return rk_conf_;
    else
      return NULL;
  }

  struct rd_kafka_topic_conf_s *c_ptr_topic() {
    if (conf_type_ == CONF_TOPIC)
      return rkt_conf_;
    else
      return NULL;
  }

  ConsumeCb *consume_cb_;
  DeliveryReportCb *dr_cb_;
  EventCb *event_cb_;
  SocketCb *socket_cb_;
  OpenCb *open_cb_;
  PartitionerCb *partitioner_cb_;
  PartitionerKeyPointerCb *partitioner_kp_cb_;
  RebalanceCb *rebalance_cb_;
  OffsetCommitCb *offset_commit_cb_;
  OAuthBearerTokenRefreshCb *oauthbearer_token_refresh_cb_;
  SslCertificateVerifyCb *ssl_cert_verify_cb_;
  ConfType conf_type_;
  rd_kafka_conf_t *rk_conf_;
  rd_kafka_topic_conf_t *rkt_conf_;
};


class HandleImpl : virtual public Handle {
 public:
  ~HandleImpl() {
  }
  HandleImpl() {
  }
  std::string name() const {
    return std::string(rd_kafka_name(rk_));
  }
  std::string memberid() const {
    char *str            = rd_kafka_memberid(rk_);
    std::string memberid = str ? str : "";
    if (str)
      rd_kafka_mem_free(rk_, str);
    return memberid;
  }
  int poll(int timeout_ms) {
    return rd_kafka_poll(rk_, timeout_ms);
  }
  int outq_len() {
    return rd_kafka_outq_len(rk_);
  }

  void set_common_config(const RdKafka::ConfImpl *confimpl);

  RdKafka::ErrorCode metadata(bool all_topics,
                              const Topic *only_rkt,
                              Metadata **metadatap,
                              int timeout_ms);

  ErrorCode pause(std::vector<TopicPartition *> &partitions);
  ErrorCode resume(std::vector<TopicPartition *> &partitions);

  ErrorCode query_watermark_offsets(const std::string &topic,
                                    int32_t partition,
                                    int64_t *low,
                                    int64_t *high,
                                    int timeout_ms) {
    return static_cast<RdKafka::ErrorCode>(rd_kafka_query_watermark_offsets(
        rk_, topic.c_str(), partition, low, high, timeout_ms));
  }

  ErrorCode get_watermark_offsets(const std::string &topic,
                                  int32_t partition,
                                  int64_t *low,
                                  int64_t *high) {
    return static_cast<RdKafka::ErrorCode>(rd_kafka_get_watermark_offsets(
        rk_, topic.c_str(), partition, low, high));
  }

  Queue *get_partition_queue(const TopicPartition *partition);

  Queue *get_sasl_queue() {
    rd_kafka_queue_t *rkqu;
    rkqu = rd_kafka_queue_get_sasl(rk_);

    if (rkqu == NULL)
      return NULL;

    return new QueueImpl(rkqu);
  }

  Queue *get_background_queue() {
    rd_kafka_queue_t *rkqu;
    rkqu = rd_kafka_queue_get_background(rk_);

    if (rkqu == NULL)
      return NULL;

    return new QueueImpl(rkqu);
  }


  ErrorCode offsetsForTimes(std::vector<TopicPartition *> &offsets,
                            int timeout_ms) {
    rd_kafka_topic_partition_list_t *c_offsets = partitions_to_c_parts(offsets);
    ErrorCode err                              = static_cast<ErrorCode>(
        rd_kafka_offsets_for_times(rk_, c_offsets, timeout_ms));
    update_partitions_from_c_parts(offsets, c_offsets);
    rd_kafka_topic_partition_list_destroy(c_offsets);
    return err;
  }

  ErrorCode set_log_queue(Queue *queue);

  void yield() {
    rd_kafka_yield(rk_);
  }

  std::string clusterid(int timeout_ms) {
    char *str             = rd_kafka_clusterid(rk_, timeout_ms);
    std::string clusterid = str ? str : "";
    if (str)
      rd_kafka_mem_free(rk_, str);
    return clusterid;
  }

  struct rd_kafka_s *c_ptr() {
    return rk_;
  }

  int32_t controllerid(int timeout_ms) {
    return rd_kafka_controllerid(rk_, timeout_ms);
  }

  ErrorCode fatal_error(std::string &errstr) const {
    char errbuf[512];
    RdKafka::ErrorCode err = static_cast<RdKafka::ErrorCode>(
        rd_kafka_fatal_error(rk_, errbuf, sizeof(errbuf)));
    if (err)
      errstr = errbuf;
    return err;
  }

  ErrorCode oauthbearer_set_token(const std::string &token_value,
                                  int64_t md_lifetime_ms,
                                  const std::string &md_principal_name,
                                  const std::list<std::string> &extensions,
                                  std::string &errstr) {
    char errbuf[512];
    ErrorCode err;
    const char **extensions_copy = new const char *[extensions.size()];
    int elem                     = 0;

    for (std::list<std::string>::const_iterator it = extensions.begin();
         it != extensions.end(); it++)
      extensions_copy[elem++] = it->c_str();
    err = static_cast<ErrorCode>(rd_kafka_oauthbearer_set_token(
        rk_, token_value.c_str(), md_lifetime_ms, md_principal_name.c_str(),
        extensions_copy, extensions.size(), errbuf, sizeof(errbuf)));
    delete[] extensions_copy;

    if (err != ERR_NO_ERROR)
      errstr = errbuf;

    return err;
  }

  ErrorCode oauthbearer_set_token_failure(const std::string &errstr) {
    return static_cast<ErrorCode>(
        rd_kafka_oauthbearer_set_token_failure(rk_, errstr.c_str()));
  }

  Error *sasl_background_callbacks_enable() {
    rd_kafka_error_t *c_error = rd_kafka_sasl_background_callbacks_enable(rk_);

    if (c_error)
      return new ErrorImpl(c_error);

    return NULL;
  }

  Error *sasl_set_credentials(const std::string &username,
                              const std::string &password) {
    rd_kafka_error_t *c_error =
        rd_kafka_sasl_set_credentials(rk_, username.c_str(), password.c_str());

    if (c_error)
      return new ErrorImpl(c_error);

    return NULL;
  };

  void *mem_malloc(size_t size) {
    return rd_kafka_mem_malloc(rk_, size);
  }

  void mem_free(void *ptr) {
    rd_kafka_mem_free(rk_, ptr);
  }

  rd_kafka_t *rk_;
  /* All Producer and Consumer callbacks must reside in HandleImpl and
   * the opaque provided to rdkafka must be a pointer to HandleImpl, since
   * ProducerImpl and ConsumerImpl classes cannot be safely directly cast to
   * HandleImpl due to the skewed diamond inheritance. */
  ConsumeCb *consume_cb_;
  EventCb *event_cb_;
  SocketCb *socket_cb_;
  OpenCb *open_cb_;
  DeliveryReportCb *dr_cb_;
  PartitionerCb *partitioner_cb_;
  PartitionerKeyPointerCb *partitioner_kp_cb_;
  RebalanceCb *rebalance_cb_;
  OffsetCommitCb *offset_commit_cb_;
  OAuthBearerTokenRefreshCb *oauthbearer_token_refresh_cb_;
  SslCertificateVerifyCb *ssl_cert_verify_cb_;
};


class TopicImpl : public Topic {
 public:
  ~TopicImpl() {
    rd_kafka_topic_destroy(rkt_);
  }

  std::string name() const {
    return rd_kafka_topic_name(rkt_);
  }

  bool partition_available(int32_t partition) const {
    return !!rd_kafka_topic_partition_available(rkt_, partition);
  }

  ErrorCode offset_store(int32_t partition, int64_t offset) {
    return static_cast<RdKafka::ErrorCode>(
        rd_kafka_offset_store(rkt_, partition, offset));
  }

  static Topic *create(Handle &base, const std::string &topic, Conf *conf);

  struct rd_kafka_topic_s *c_ptr() {
    return rkt_;
  }

  rd_kafka_topic_t *rkt_;
  PartitionerCb *partitioner_cb_;
  PartitionerKeyPointerCb *partitioner_kp_cb_;
};


/**
 * Topic and Partition
 */
class TopicPartitionImpl : public TopicPartition {
 public:
  ~TopicPartitionImpl() {
  }

  static TopicPartition *create(const std::string &topic, int partition);

  TopicPartitionImpl(const std::string &topic, int partition) :
      topic_(topic),
      partition_(partition),
      offset_(RdKafka::Topic::OFFSET_INVALID),
      err_(ERR_NO_ERROR) {
  }

  TopicPartitionImpl(const std::string &topic, int partition, int64_t offset) :
      topic_(topic),
      partition_(partition),
      offset_(offset),
      err_(ERR_NO_ERROR) {
  }

  TopicPartitionImpl(const rd_kafka_topic_partition_t *c_part) {
    topic_     = std::string(c_part->topic);
    partition_ = c_part->partition;
    offset_    = c_part->offset;
    err_       = static_cast<ErrorCode>(c_part->err);
    // FIXME: metadata
  }

  static void destroy(std::vector<TopicPartition *> &partitions);

  int partition() const {
    return partition_;
  }
  const std::string &topic() const {
    return topic_;
  }

  int64_t offset() const {
    return offset_;
  }

  ErrorCode err() const {
    return err_;
  }

  void set_offset(int64_t offset) {
    offset_ = offset;
  }

  std::ostream &operator<<(std::ostream &ostrm) const {
    return ostrm << topic_ << " [" << partition_ << "]";
  }

  std::string topic_;
  int partition_;
  int64_t offset_;
  ErrorCode err_;
};


/**
 * @class ConsumerGroupMetadata wraps the
 *        C rd_kafka_consumer_group_metadata_t object.
 */
class ConsumerGroupMetadataImpl : public ConsumerGroupMetadata {
 public:
  ~ConsumerGroupMetadataImpl() {
    rd_kafka_consumer_group_metadata_destroy(cgmetadata_);
  }

  ConsumerGroupMetadataImpl(rd_kafka_consumer_group_metadata_t *cgmetadata) :
      cgmetadata_(cgmetadata) {
  }

  rd_kafka_consumer_group_metadata_t *cgmetadata_;
};


class KafkaConsumerImpl : virtual public KafkaConsumer,
                          virtual public HandleImpl {
 public:
  ~KafkaConsumerImpl() {
    if (rk_)
      rd_kafka_destroy_flags(rk_, RD_KAFKA_DESTROY_F_NO_CONSUMER_CLOSE);
  }

  static KafkaConsumer *create(Conf *conf, std::string &errstr);

  ErrorCode assignment(std::vector<TopicPartition *> &partitions);
  bool assignment_lost();
  std::string rebalance_protocol() {
    const char *str = rd_kafka_rebalance_protocol(rk_);
    return std::string(str ? str : "");
  }
  ErrorCode subscription(std::vector<std::string> &topics);
  ErrorCode subscribe(const std::vector<std::string> &topics);
  ErrorCode unsubscribe();
  ErrorCode assign(const std::vector<TopicPartition *> &partitions);
  ErrorCode unassign();
  Error *incremental_assign(const std::vector<TopicPartition *> &partitions);
  Error *incremental_unassign(const std::vector<TopicPartition *> &partitions);

  Message *consume(int timeout_ms);
  ErrorCode commitSync() {
    return static_cast<ErrorCode>(rd_kafka_commit(rk_, NULL, 0 /*sync*/));
  }
  ErrorCode commitAsync() {
    return static_cast<ErrorCode>(rd_kafka_commit(rk_, NULL, 1 /*async*/));
  }
  ErrorCode commitSync(Message *message) {
    MessageImpl *msgimpl = dynamic_cast<MessageImpl *>(message);
    return static_cast<ErrorCode>(
        rd_kafka_commit_message(rk_, msgimpl->rkmessage_, 0 /*sync*/));
  }
  ErrorCode commitAsync(Message *message) {
    MessageImpl *msgimpl = dynamic_cast<MessageImpl *>(message);
    return static_cast<ErrorCode>(
        rd_kafka_commit_message(rk_, msgimpl->rkmessage_, 1 /*async*/));
  }

  ErrorCode commitSync(std::vector<TopicPartition *> &offsets) {
    rd_kafka_topic_partition_list_t *c_parts = partitions_to_c_parts(offsets);
    rd_kafka_resp_err_t err                  = rd_kafka_commit(rk_, c_parts, 0);
    if (!err)
      update_partitions_from_c_parts(offsets, c_parts);
    rd_kafka_topic_partition_list_destroy(c_parts);
    return static_cast<ErrorCode>(err);
  }

  ErrorCode commitAsync(const std::vector<TopicPartition *> &offsets) {
    rd_kafka_topic_partition_list_t *c_parts = partitions_to_c_parts(offsets);
    rd_kafka_resp_err_t err                  = rd_kafka_commit(rk_, c_parts, 1);
    rd_kafka_topic_partition_list_destroy(c_parts);
    return static_cast<ErrorCode>(err);
  }

  ErrorCode commitSync(OffsetCommitCb *offset_commit_cb) {
    return static_cast<ErrorCode>(rd_kafka_commit_queue(
        rk_, NULL, NULL, RdKafka::offset_commit_cb_trampoline0,
        offset_commit_cb));
  }

  ErrorCode commitSync(std::vector<TopicPartition *> &offsets,
                       OffsetCommitCb *offset_commit_cb) {
    rd_kafka_topic_partition_list_t *c_parts = partitions_to_c_parts(offsets);
    rd_kafka_resp_err_t err                  = rd_kafka_commit_queue(
        rk_, c_parts, NULL, RdKafka::offset_commit_cb_trampoline0,
        offset_commit_cb);
    rd_kafka_topic_partition_list_destroy(c_parts);
    return static_cast<ErrorCode>(err);
  }

  ErrorCode committed(std::vector<TopicPartition *> &partitions,
                      int timeout_ms);
  ErrorCode position(std::vector<TopicPartition *> &partitions);

  ConsumerGroupMetadata *groupMetadata() {
    rd_kafka_consumer_group_metadata_t *cgmetadata;

    cgmetadata = rd_kafka_consumer_group_metadata(rk_);
    if (!cgmetadata)
      return NULL;

    return new ConsumerGroupMetadataImpl(cgmetadata);
  }

  ErrorCode close();

  Error *close(Queue *queue);

  bool closed() {
    return rd_kafka_consumer_closed(rk_) ? true : false;
  }

  ErrorCode seek(const TopicPartition &partition, int timeout_ms);

  ErrorCode offsets_store(std::vector<TopicPartition *> &offsets) {
    rd_kafka_topic_partition_list_t *c_parts = partitions_to_c_parts(offsets);
    rd_kafka_resp_err_t err = rd_kafka_offsets_store(rk_, c_parts);
    update_partitions_from_c_parts(offsets, c_parts);
    rd_kafka_topic_partition_list_destroy(c_parts);
    return static_cast<ErrorCode>(err);
  }
};


class MetadataImpl : public Metadata {
 public:
  MetadataImpl(const rd_kafka_metadata_t *metadata);
  ~MetadataImpl();

  const std::vector<const BrokerMetadata *> *brokers() const {
    return &brokers_;
  }

  const std::vector<const TopicMetadata *> *topics() const {
    return &topics_;
  }

  std::string orig_broker_name() const {
    return std::string(metadata_->orig_broker_name);
  }

  int32_t orig_broker_id() const {
    return metadata_->orig_broker_id;
  }

 private:
  const rd_kafka_metadata_t *metadata_;
  std::vector<const BrokerMetadata *> brokers_;
  std::vector<const TopicMetadata *> topics_;
  std::string orig_broker_name_;
};



class ConsumerImpl : virtual public Consumer, virtual public HandleImpl {
 public:
  ~ConsumerImpl() {
    if (rk_)
      rd_kafka_destroy(rk_);
  }
  static Consumer *create(Conf *conf, std::string &errstr);

  ErrorCode start(Topic *topic, int32_t partition, int64_t offset);
  ErrorCode start(Topic *topic,
                  int32_t partition,
                  int64_t offset,
                  Queue *queue);
  ErrorCode stop(Topic *topic, int32_t partition);
  ErrorCode seek(Topic *topic,
                 int32_t partition,
                 int64_t offset,
                 int timeout_ms);
  Message *consume(Topic *topic, int32_t partition, int timeout_ms);
  Message *consume(Queue *queue, int timeout_ms);
  int consume_callback(Topic *topic,
                       int32_t partition,
                       int timeout_ms,
                       ConsumeCb *cb,
                       void *opaque);
  int consume_callback(Queue *queue,
                       int timeout_ms,
                       RdKafka::ConsumeCb *consume_cb,
                       void *opaque);
};



class ProducerImpl : virtual public Producer, virtual public HandleImpl {
 public:
  ~ProducerImpl() {
    if (rk_)
      rd_kafka_destroy(rk_);
  }

  ErrorCode produce(Topic *topic,
                    int32_t partition,
                    int msgflags,
                    void *payload,
                    size_t len,
                    const std::string *key,
                    void *msg_opaque);

  ErrorCode produce(Topic *topic,
                    int32_t partition,
                    int msgflags,
                    void *payload,
                    size_t len,
                    const void *key,
                    size_t key_len,
                    void *msg_opaque);

  ErrorCode produce(Topic *topic,
                    int32_t partition,
                    const std::vector<char> *payload,
                    const std::vector<char> *key,
                    void *msg_opaque);

  ErrorCode produce(const std::string topic_name,
                    int32_t partition,
                    int msgflags,
                    void *payload,
                    size_t len,
                    const void *key,
                    size_t key_len,
                    int64_t timestamp,
                    void *msg_opaque);

  ErrorCode produce(const std::string topic_name,
                    int32_t partition,
                    int msgflags,
                    void *payload,
                    size_t len,
                    const void *key,
                    size_t key_len,
                    int64_t timestamp,
                    RdKafka::Headers *headers,
                    void *msg_opaque);

  ErrorCode flush(int timeout_ms) {
    return static_cast<RdKafka::ErrorCode>(rd_kafka_flush(rk_, timeout_ms));
  }

  ErrorCode purge(int purge_flags) {
    return static_cast<RdKafka::ErrorCode>(
        rd_kafka_purge(rk_, (int)purge_flags));
  }

  Error *init_transactions(int timeout_ms) {
    rd_kafka_error_t *c_error;

    c_error = rd_kafka_init_transactions(rk_, timeout_ms);

    if (c_error)
      return new ErrorImpl(c_error);
    else
      return NULL;
  }

  Error *begin_transaction() {
    rd_kafka_error_t *c_error;

    c_error = rd_kafka_begin_transaction(rk_);

    if (c_error)
      return new ErrorImpl(c_error);
    else
      return NULL;
  }

  Error *send_offsets_to_transaction(
      const std::vector<TopicPartition *> &offsets,
      const ConsumerGroupMetadata *group_metadata,
      int timeout_ms) {
    rd_kafka_error_t *c_error;
    const RdKafka::ConsumerGroupMetadataImpl *cgmdimpl =
        dynamic_cast<const RdKafka::ConsumerGroupMetadataImpl *>(
            group_metadata);
    rd_kafka_topic_partition_list_t *c_offsets = partitions_to_c_parts(offsets);

    c_error = rd_kafka_send_offsets_to_transaction(
        rk_, c_offsets, cgmdimpl->cgmetadata_, timeout_ms);

    rd_kafka_topic_partition_list_destroy(c_offsets);

    if (c_error)
      return new ErrorImpl(c_error);
    else
      return NULL;
  }

  Error *commit_transaction(int timeout_ms) {
    rd_kafka_error_t *c_error;

    c_error = rd_kafka_commit_transaction(rk_, timeout_ms);

    if (c_error)
      return new ErrorImpl(c_error);
    else
      return NULL;
  }

  Error *abort_transaction(int timeout_ms) {
    rd_kafka_error_t *c_error;

    c_error = rd_kafka_abort_transaction(rk_, timeout_ms);

    if (c_error)
      return new ErrorImpl(c_error);
    else
      return NULL;
  }

  static Producer *create(Conf *conf, std::string &errstr);
};



}  // namespace RdKafka

#endif /* _RDKAFKACPP_INT_H_ */
