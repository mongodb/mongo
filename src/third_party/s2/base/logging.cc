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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kGeo

#include "logging.h"

#include <utility>

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

using ::mongo::logger::LogstreamBuilder;

LogMessageBase::LogMessageBase(LogstreamBuilder builder, const char* file, int line) :
    _lsb(std::move(builder)) {
    _lsb.setBaseMessage(mongoutils::str::stream() << file << ':' << line << ": ");
}

LogMessageBase::LogMessageBase(LogstreamBuilder builder) : _lsb(std::move(builder)) { }

LogMessageInfo::LogMessageInfo() : LogMessageBase(mongo::log()) { }

LogMessageWarning::LogMessageWarning(const char* file, int line) :
        LogMessageBase(mongo::warning(), file, line) { }

LogMessageFatal::LogMessageFatal(const char* file, int line) :
        LogMessageBase(mongo::severe(), file, line) { }

LogMessageFatal::~LogMessageFatal() {
    _lsb.~LogstreamBuilder();
    fassertFailed(40048);
}
