// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/diagnostics/logger.hpp"

#include "azure/core/internal/diagnostics/log.hpp"
#include "private/environment_log_level_listener.hpp"

#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <sstream>

using namespace Azure::Core::Diagnostics;
using namespace Azure::Core::Diagnostics::_internal;

namespace {
static std::shared_timed_mutex g_logListenerMutex;
static std::function<void(Logger::Level level, std::string const& message)> g_logListener(
    _detail::EnvironmentLogLevelListener::GetLogListener());
} // namespace

std::atomic<bool> Log::g_isLoggingEnabled(
    _detail::EnvironmentLogLevelListener::GetLogListener() != nullptr);

std::atomic<Logger::Level> Log::g_logLevel(
    _detail::EnvironmentLogLevelListener::GetLogLevel(Logger::Level::Warning));

inline void Log::EnableLogging(bool isEnabled) { g_isLoggingEnabled = isEnabled; }

inline void Log::SetLogLevel(Logger::Level logLevel) { g_logLevel = logLevel; }

void Log::Write(Logger::Level level, std::string const& message)
{
  if (ShouldWrite(level) && !message.empty())
  {
    std::shared_lock<std::shared_timed_mutex> loggerLock(g_logListenerMutex);
    if (g_logListener)
    {
      g_logListener(level, message);
    }
  }
}

void Logger::SetListener(
    std::function<void(Logger::Level level, std::string const& message)> listener)
{
  std::unique_lock<std::shared_timed_mutex> loggerLock(g_logListenerMutex);
  g_logListener = std::move(listener);
  Log::EnableLogging(g_logListener != nullptr);
}

void Logger::SetLevel(Logger::Level level) { Log::SetLogLevel(level); }
