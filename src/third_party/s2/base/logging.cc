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

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

LogMessageInfo::LogMessageInfo() : _lsb(mongo::log()) { }

LogMessageFatal::LogMessageFatal(const char* file, int line) :
    _lsb(mongo::severe()) {
    _lsb.setBaseMessage(mongoutils::str::stream() << file << ':' << line << ": ");
}

LogMessageFatal::~LogMessageFatal() {
    _lsb.~LogstreamBuilder();
    mongo::fassertFailed(0);
}
