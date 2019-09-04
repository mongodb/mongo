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

#include <iosfwd>

#include "mongo/logger/log_severity.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/util/concurrency/thread_name.h"

#include "macros.h"

// Always-on checking
#define CHECK(x)	if(x){}else LogMessageFatal(__FILE__, __LINE__).stream() << "Check failed: " #x
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
#define DCHECK(x) if(x){}else LogMessageWarning(__FILE__, __LINE__).stream() << "Check failed: " #x
#define DCHECK_LT(x, y)  DCHECK((x) < (y))
#define DCHECK_GT(x, y)  DCHECK((x) > (y))
#define DCHECK_LE(x, y)  DCHECK((x) <= (y))
#define DCHECK_GE(x, y)  DCHECK((x) >= (y))
#define DCHECK_EQ(x, y)  DCHECK((x) == (y))
#define DCHECK_NE(x, y)  DCHECK((x) != (y))
#endif

#include "base/port.h"
#define INFO LogMessageInfo().stream()
#define WARN LogMessageWarning(__FILE__, __LINE__).stream()
#define FATAL LogMessageFatal(__FILE__, __LINE__).stream()
#define DFATAL LogMessageFatal(__FILE__, __LINE__).stream()

// VLOG messages will be logged at debug level 5 with the S2 log component.
#define S2LOG(x) x
// Expansion of MONGO_LOG_COMPONENT defined in mongo/util/log.h
#define VLOG(x) \
    if (!(::mongo::logger::globalLogDomain())->shouldLog(::mongo::logger::LogComponent::kGeo, ::mongo::logger::LogSeverity::Debug(5))) {} \
    else ::mongo::logger::LogstreamBuilder(::mongo::logger::globalLogDomain(), ::mongo::getThreadName(), ::mongo::logger::LogSeverity::Debug(5), ::mongo::logger::LogComponent::kGeo)

class LogMessageBase {
public:
    LogMessageBase(::mongo::logger::LogstreamBuilder builder);
    LogMessageBase(::mongo::logger::LogstreamBuilder builder, const char* file, int line);
    virtual ~LogMessageBase() { };
    std::ostream& stream() { return _lsb.stream(); }
protected:
    // Fatal message will deconstruct it before abort to flush final message.
    mongo::logger::LogstreamBuilder _lsb;
private:
    DISALLOW_COPY_AND_ASSIGN(LogMessageBase);
};

class LogMessageInfo : public LogMessageBase {
public:
    LogMessageInfo();
    virtual ~LogMessageInfo() { };

private:
    DISALLOW_COPY_AND_ASSIGN(LogMessageInfo);
};

class LogMessageWarning : public LogMessageBase {
public:
    LogMessageWarning(const char* file, int line);
    virtual ~LogMessageWarning() { };

private:
    DISALLOW_COPY_AND_ASSIGN(LogMessageWarning);
};

class LogMessageFatal : public LogMessageBase {
public:
    LogMessageFatal(const char* file, int line);
    virtual ~LogMessageFatal();

private:
    DISALLOW_COPY_AND_ASSIGN(LogMessageFatal);
};

#endif  // BASE_LOGGING_H
