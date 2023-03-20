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



RdKafka::ConfImpl::ConfResult RdKafka::ConfImpl::set(const std::string &name,
                                                     const std::string &value,
                                                     std::string &errstr) {
  rd_kafka_conf_res_t res;
  char errbuf[512];

  if (this->conf_type_ == CONF_GLOBAL)
    res = rd_kafka_conf_set(this->rk_conf_, name.c_str(), value.c_str(), errbuf,
                            sizeof(errbuf));
  else
    res = rd_kafka_topic_conf_set(this->rkt_conf_, name.c_str(), value.c_str(),
                                  errbuf, sizeof(errbuf));

  if (res != RD_KAFKA_CONF_OK)
    errstr = errbuf;

  return static_cast<Conf::ConfResult>(res);
}


std::list<std::string> *RdKafka::ConfImpl::dump() {
  const char **arrc;
  size_t cnt;
  std::list<std::string> *arr;

  if (rk_conf_)
    arrc = rd_kafka_conf_dump(rk_conf_, &cnt);
  else
    arrc = rd_kafka_topic_conf_dump(rkt_conf_, &cnt);

  arr = new std::list<std::string>();
  for (int i = 0; i < static_cast<int>(cnt); i++)
    arr->push_back(std::string(arrc[i]));

  rd_kafka_conf_dump_free(arrc, cnt);
  return arr;
}

RdKafka::Conf *RdKafka::Conf::create(ConfType type) {
  ConfImpl *conf = new ConfImpl(type);

  if (type == CONF_GLOBAL)
    conf->rk_conf_ = rd_kafka_conf_new();
  else
    conf->rkt_conf_ = rd_kafka_topic_conf_new();

  return conf;
}
