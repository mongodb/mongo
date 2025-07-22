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

#include "rdkafkacpp_int.h"

using namespace RdKafka;

BrokerMetadata::~BrokerMetadata() {
}
PartitionMetadata::~PartitionMetadata() {
}
TopicMetadata::~TopicMetadata() {
}
Metadata::~Metadata() {
}


/**
 * Metadata: Broker information handler implementation
 */
class BrokerMetadataImpl : public BrokerMetadata {
 public:
  BrokerMetadataImpl(const rd_kafka_metadata_broker_t *broker_metadata) :
      broker_metadata_(broker_metadata), host_(broker_metadata->host) {
  }

  int32_t id() const {
    return broker_metadata_->id;
  }

  std::string host() const {
    return host_;
  }
  int port() const {
    return broker_metadata_->port;
  }

  virtual ~BrokerMetadataImpl() {
  }

 private:
  const rd_kafka_metadata_broker_t *broker_metadata_;
  const std::string host_;
};

/**
 * Metadata: Partition information handler
 */
class PartitionMetadataImpl : public PartitionMetadata {
 public:
  // @TODO too much memory copy? maybe we should create a new vector class that
  // read directly from C arrays?
  // @TODO use auto_ptr?
  PartitionMetadataImpl(
      const rd_kafka_metadata_partition_t *partition_metadata) :
      partition_metadata_(partition_metadata) {
    replicas_.reserve(partition_metadata->replica_cnt);
    for (int i = 0; i < partition_metadata->replica_cnt; ++i)
      replicas_.push_back(partition_metadata->replicas[i]);

    isrs_.reserve(partition_metadata->isr_cnt);
    for (int i = 0; i < partition_metadata->isr_cnt; ++i)
      isrs_.push_back(partition_metadata->isrs[i]);
  }

  int32_t id() const {
    return partition_metadata_->id;
  }
  int32_t leader() const {
    return partition_metadata_->leader;
  }
  ErrorCode err() const {
    return static_cast<ErrorCode>(partition_metadata_->err);
  }

  const std::vector<int32_t> *replicas() const {
    return &replicas_;
  }
  const std::vector<int32_t> *isrs() const {
    return &isrs_;
  }

  ~PartitionMetadataImpl() {
  }

 private:
  const rd_kafka_metadata_partition_t *partition_metadata_;
  std::vector<int32_t> replicas_, isrs_;
};

/**
 * Metadata: Topic information handler
 */
class TopicMetadataImpl : public TopicMetadata {
 public:
  TopicMetadataImpl(const rd_kafka_metadata_topic_t *topic_metadata) :
      topic_metadata_(topic_metadata), topic_(topic_metadata->topic) {
    partitions_.reserve(topic_metadata->partition_cnt);
    for (int i = 0; i < topic_metadata->partition_cnt; ++i)
      partitions_.push_back(
          new PartitionMetadataImpl(&topic_metadata->partitions[i]));
  }

  ~TopicMetadataImpl() {
    for (size_t i = 0; i < partitions_.size(); ++i)
      delete partitions_[i];
  }

  std::string topic() const {
    return topic_;
  }
  const std::vector<const PartitionMetadata *> *partitions() const {
    return &partitions_;
  }
  ErrorCode err() const {
    return static_cast<ErrorCode>(topic_metadata_->err);
  }

 private:
  const rd_kafka_metadata_topic_t *topic_metadata_;
  const std::string topic_;
  std::vector<const PartitionMetadata *> partitions_;
};

MetadataImpl::MetadataImpl(const rd_kafka_metadata_t *metadata) :
    metadata_(metadata) {
  brokers_.reserve(metadata->broker_cnt);
  for (int i = 0; i < metadata->broker_cnt; ++i)
    brokers_.push_back(new BrokerMetadataImpl(&metadata->brokers[i]));

  topics_.reserve(metadata->topic_cnt);
  for (int i = 0; i < metadata->topic_cnt; ++i)
    topics_.push_back(new TopicMetadataImpl(&metadata->topics[i]));
}

MetadataImpl::~MetadataImpl() {
  for (size_t i = 0; i < brokers_.size(); ++i)
    delete brokers_[i];
  for (size_t i = 0; i < topics_.size(); ++i)
    delete topics_[i];


  if (metadata_)
    rd_kafka_metadata_destroy(metadata_);
}
