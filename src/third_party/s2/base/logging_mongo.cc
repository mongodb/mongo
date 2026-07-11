// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "logging.h"

#include <boost/optional.hpp>
#include <utility>

#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kGeo


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
