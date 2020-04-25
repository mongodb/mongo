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

#include "logging.h"

#include <utility>

namespace s2_env {

LoggingEnv::~LoggingEnv() = default;

LogMessageSink::~LogMessageSink() = default;

LogMessage::LogMessage(int verbosity)
  : _sink(globalLoggingEnv().makeSink(verbosity)) { }
LogMessage::LogMessage(Severity severity)
  : _sink(globalLoggingEnv().makeSink(severity)) { }
LogMessage::LogMessage(Severity severity, const char* file, int line)
  : _sink(globalLoggingEnv().makeSink(severity, file, line)) { }

LogMessage::~LogMessage() = default;

}  // namespace s2_env
