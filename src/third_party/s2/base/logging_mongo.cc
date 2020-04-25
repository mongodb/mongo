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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kGeo

#include "logging.h"

#include <boost/optional.hpp>
#include <utility>

#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"

namespace s2_mongo {

// VLOG messages will be logged at debug level 5 with the S2 log component.
// Expansion of MONGO_LOG_COMPONENT defined in mongo/util/log.h
class VLogSink : public s2_env::LogMessageSink {
public:
  explicit VLogSink(int verbosity)
    : _v(verbosity) {}
    ~VLogSink() {
      using namespace mongo::literals;
      LOGV2_DEBUG_OPTIONS(
        25000, 5, {mongo::logv2::LogComponent::kGeo}, "{message}", "message"_attr = _os.str());
  }

  std::ostream& stream() override { return _os; }

private:
  int _v;
  std::ostringstream _os;
};

class SeverityLogSink : public s2_env::LogMessageSink {
public:
  explicit SeverityLogSink(s2_env::LogMessage::Severity severity)
    : _severity(severity) {}

  SeverityLogSink(s2_env::LogMessage::Severity severity,
                  const char* file, int line)
    : _severity(severity) {
    _os << file << ":" << line << ": ";
  }
  ~SeverityLogSink() {
    auto severity = mongo::logv2::LogSeverity::Log();
    switch (_severity) {
      case s2_env::LogMessage::Severity::kInfo:
        break;
      case s2_env::LogMessage::Severity::kWarning:
        severity = mongo::logv2::LogSeverity::Warning();
        break;
      case s2_env::LogMessage::Severity::kFatal:
      default:
        severity = mongo::logv2::LogSeverity::Severe();
        break;
    };
    using namespace mongo::literals;
    LOGV2_IMPL(
      25001, severity, {mongo::logv2::LogComponent::kGeo}, "{message}", "message"_attr = _os.str());
    if (_severity == s2_env::LogMessage::Severity::kFatal) {
      fassertFailed(40048);
    }
  }

  std::ostream& stream() override { return _os; }
private:
  s2_env::LogMessage::Severity _severity;
  std::ostringstream _os;
};

template <typename...A>
std::unique_ptr<s2_env::LogMessageSink> makeSinkImpl(s2_env::LogMessage::Severity severity, A&&...a) {
  return std::make_unique<SeverityLogSink>(severity, std::forward<A>(a)...);
}

class MongoLoggingEnv : public s2_env::LoggingEnv {
public:
  MongoLoggingEnv() = default;
  ~MongoLoggingEnv() override {}
  bool shouldVLog(int verbosity) override {
    return mongo::logv2::LogManager::global().getGlobalSettings().shouldLog(
      mongo::logv2::LogComponent::kGeo, mongo::logv2::LogSeverity::Debug(5));
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
