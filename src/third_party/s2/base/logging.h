// Copyright 2010 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BASE_LOGGING_H
#define BASE_LOGGING_H

#include <iostream>
#include <sstream>
#include <memory>

#include "macros.h"

// Always-on checking
#define CHECK(x)	if(x){}else FATAL << "Check failed: " #x
#define CHECK_LT(x, y)	CHECK((x) < (y))
#define CHECK_GT(x, y)	CHECK((x) > (y))
#define CHECK_LE(x, y)	CHECK((x) <= (y))
#define CHECK_GE(x, y)	CHECK((x) >= (y))
#define CHECK_EQ(x, y)	CHECK((x) == (y))
#define CHECK_NE(x, y)	CHECK((x) != (y))
#define CHECK_NOTNULL(x) CHECK((x) != NULL)

#ifdef MONGO_CONFIG_DEBUG_BUILD
// Checking which is only fatal in debug mode
#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(val1, val2) CHECK_EQ(val1, val2)
#define DCHECK_NE(val1, val2) CHECK_NE(val1, val2)
#define DCHECK_LE(val1, val2) CHECK_LE(val1, val2)
#define DCHECK_LT(val1, val2) CHECK_LT(val1, val2)
#define DCHECK_GE(val1, val2) CHECK_GE(val1, val2)
#define DCHECK_GT(val1, val2) CHECK_GT(val1, val2)
#else
#define DCHECK(x) if(x){}else WARN << "Check failed: " #x
#define DCHECK_LT(x, y)  DCHECK((x) < (y))
#define DCHECK_GT(x, y)  DCHECK((x) > (y))
#define DCHECK_LE(x, y)  DCHECK((x) <= (y))
#define DCHECK_GE(x, y)  DCHECK((x) >= (y))
#define DCHECK_EQ(x, y)  DCHECK((x) == (y))
#define DCHECK_NE(x, y)  DCHECK((x) != (y))
#endif

#include "base/port.h"
#define INFO s2_env::LogMessage(s2_env::LogMessage::Severity::kInfo).stream()
#define WARN s2_env::LogMessage(s2_env::LogMessage::Severity::kWarning, __FILE__, __LINE__).stream()
#define FATAL s2_env::LogMessage(s2_env::LogMessage::Severity::kFatal, __FILE__, __LINE__).stream()
#define DFATAL s2_env::LogMessage(s2_env::LogMessage::Severity::kFatal, __FILE__, __LINE__).stream()

#define S2LOG(x) x
#define VLOG(x) \
  if (!s2_env::globalLoggingEnv().shouldVLog((int)x)) { \
  } else s2_env::LogMessage(int(x)).stream()

namespace s2_env {

class LogMessageSink {
public:
  virtual ~LogMessageSink();
  virtual std::ostream& stream() = 0;
};

class LogMessage {
public:
  enum class Severity {
    kInfo,
    kWarning,
    kFatal,
  };

  explicit LogMessage(int verbosity);
  explicit LogMessage(Severity severity);
  explicit LogMessage(Severity severity, const char* file, int line);

  virtual ~LogMessage();

  std::ostream& stream() {
    return _sink->stream();
  }

protected:
  std::unique_ptr<LogMessageSink> _sink;
};

class StringStream {
public:
  StringStream() = default;

  template <typename T>
  StringStream& operator<<(T&& t) {
    _s << std::forward<T>(t);
    return *this;
  }

  operator std::string() const {
    return _s.str();
  }

private:
  std::ostringstream _s;
};

class LoggingEnv {
public:
  virtual ~LoggingEnv();
  virtual bool shouldVLog(int verbosity) = 0;
  virtual std::unique_ptr<LogMessageSink> makeSink(int verbosity) = 0;
  virtual std::unique_ptr<LogMessageSink> makeSink(LogMessage::Severity severity) = 0;
  virtual std::unique_ptr<LogMessageSink> makeSink(LogMessage::Severity severity,
                                                   const char* file, int line) = 0;
};

// provided by logging_mongo.cc
LoggingEnv& globalLoggingEnv();

}  // namespace s2_env
#endif  // BASE_LOGGING_H
