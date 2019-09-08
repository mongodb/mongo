/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kGeo

#include "logging.h"

#include <boost/optional.hpp>
#include <utility>

#include "mongo/logger/logger.h"
#include "mongo/logger/log_severity.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"

namespace s2_mongo {

namespace ml = mongo::logger;

// VLOG messages will be logged at debug level 5 with the S2 log component.
// Expansion of MONGO_LOG_COMPONENT defined in mongo/util/log.h
class VLogSink : public s2_env::LogMessageSink {
public:
  explicit VLogSink(int verbosity)
    : _v(verbosity),
      _lsb(ml::globalLogDomain(),
           mongo::getThreadName(),
           ml::LogSeverity::Debug(5),
           ml::LogComponent::kGeo) {}
  std::ostream& stream() override { return _lsb.stream(); }
private:
  int _v;
  ml::LogstreamBuilder _lsb;
};

class SeverityLogSink : public s2_env::LogMessageSink {
public:
  // Fatal message will deconstruct it before abort to flush final message.
  explicit SeverityLogSink(s2_env::LogMessage::Severity severity, ml::LogstreamBuilder builder)
    : _severity(severity),
      _lsb(std::move(builder)) {}

  SeverityLogSink(s2_env::LogMessage::Severity severity, ml::LogstreamBuilder builder,
                  const char* file, int line)
    : _severity(severity),
      _lsb(std::move(builder)) {
    std::ostringstream os;
    os << file << ":" << line << ": ";
    _lsb->setBaseMessage(os.str());
  }
  ~SeverityLogSink() {
    if (_severity == s2_env::LogMessage::Severity::kFatal) {
      _lsb = {};  // killing _lsb early to force a log flush
      fassertFailed(40048);
    }
  }

  std::ostream& stream() override { return _lsb->stream(); }
private:
  s2_env::LogMessage::Severity _severity;
  boost::optional<ml::LogstreamBuilder> _lsb;
};

template <typename...A>
std::unique_ptr<s2_env::LogMessageSink> makeSinkImpl(s2_env::LogMessage::Severity severity, A&&...a) {
  auto builder = [&] {
    switch (severity) {
    case s2_env::LogMessage::Severity::kInfo:
        return mongo::log();
    case s2_env::LogMessage::Severity::kWarning:
        return mongo::warning();
    case s2_env::LogMessage::Severity::kFatal:
    default:
        return mongo::severe();
    }
  };
  return std::make_unique<SeverityLogSink>(severity, builder(), std::forward<A>(a)...);
}

class MongoLoggingEnv : public s2_env::LoggingEnv {
public:
  MongoLoggingEnv() = default;
  ~MongoLoggingEnv() override {}
  bool shouldVLog(int verbosity) override {
    return ml::globalLogDomain()->shouldLog(
      ml::LogComponent::kGeo,
      ml::LogSeverity::Debug(5));
  }
  std::unique_ptr<s2_env::LogMessageSink> makeSink(int verbosity) override {
    return std::make_unique<VLogSink>(verbosity);
  }
  std::unique_ptr<s2_env::LogMessageSink> makeSink(s2_env::LogMessage::Severity severity) override {
    return makeSinkImpl(severity);
  }
  std::unique_ptr<s2_env::LogMessageSink> makeSink(s2_env::LogMessage::Severity severity,
                                           const char* file, int line) override {
    return makeSinkImpl(severity, file, line);
  }
};

}  // namespace s2_mongo

namespace s2_env {

LoggingEnv& globalLoggingEnv() {
  static LoggingEnv *p = new s2_mongo::MongoLoggingEnv();
  return *p;
}

}  // namespace s2_env
