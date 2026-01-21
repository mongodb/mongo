// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "private/environment_log_level_listener.hpp"

#include "azure/core/datetime.hpp"
#include "azure/core/internal/environment.hpp"
#include "azure/core/internal/strings.hpp"

#include <iostream>
#include <string>
#include <thread>

using Azure::Core::_internal::Environment;
using namespace Azure::Core::Diagnostics;
using namespace Azure::Core::Diagnostics::_detail;
using Azure::Core::Diagnostics::_detail::EnvironmentLogLevelListener;

namespace {
Logger::Level const* GetEnvironmentLogLevel()
{
  static Logger::Level* envLogLevelPtr = nullptr;

  if (!EnvironmentLogLevelListener::IsInitialized())
  {
    EnvironmentLogLevelListener::SetInitialized(true);

    auto const logLevelStr = Environment::GetVariable("AZURE_LOG_LEVEL");
    if (!logLevelStr.empty())
    {
      // See https://github.com/Azure/azure-sdk-for-java/wiki/Logging-with-Azure-SDK
      // And
      // https://github.com/Azure/azure-sdk-for-java/blob/main/sdk/core/azure-core/src/main/java/com/azure/core/util/logging/LogLevel.java
      using Azure::Core::_internal::StringExtensions;

      static Logger::Level envLogLevel = {};
      envLogLevelPtr = &envLogLevel;

      if (logLevelStr == "4"
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "error")
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "err"))
      {
        envLogLevel = Logger::Level::Error;
      }
      else if (
          logLevelStr == "3"
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "warning")
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "warn"))
      {
        envLogLevel = Logger::Level::Warning;
      }
      else if (
          logLevelStr == "2"
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "informational")
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "information")
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "info"))
      {
        envLogLevel = Logger::Level::Informational;
      }
      else if (
          logLevelStr == "1"
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "verbose")
          || StringExtensions::LocaleInvariantCaseInsensitiveEqual(logLevelStr, "debug"))
      {
        envLogLevel = Logger::Level::Verbose;
      }
      else
      {
        envLogLevelPtr = nullptr;
      }
    }
  }

  return envLogLevelPtr;
}

// Log level textual representation, including space padding, matches slf4j and log4net.
std::string const ErrorText = "ERROR";
std::string const WarningText = "WARN ";
std::string const InformationalText = "INFO ";
std::string const VerboseText = "DEBUG";
std::string const UnknownText = "?????";

inline std::string const& LogLevelToConsoleString(Logger::Level logLevel)
{
  switch (logLevel)
  {
    case Logger::Level::Error:
      return ErrorText;

    case Logger::Level::Warning:
      return WarningText;

    case Logger::Level::Informational:
      return InformationalText;

    case Logger::Level::Verbose:
      return VerboseText;

    default:
      return UnknownText;
  }
}
} // namespace

Logger::Level EnvironmentLogLevelListener::GetLogLevel(Logger::Level defaultValue)
{
  auto const envLogLevelPtr = GetEnvironmentLogLevel();
  return envLogLevelPtr ? *envLogLevelPtr : defaultValue;
}

std::function<void(Logger::Level level, std::string const& message)>
EnvironmentLogLevelListener::GetLogListener()
{
  bool const isEnvLogLevelSet = GetEnvironmentLogLevel() != nullptr;
  if (!isEnvLogLevelSet)
  {
    return nullptr;
  }

  static std::function<void(Logger::Level level, std::string const& message)> const consoleLogger =
      [](auto level, auto message) {
        // Log diagnostics to cerr rather than cout.
        std::cerr << '['
                  << Azure::DateTime(std::chrono::system_clock::now())
                         .ToString(
                             DateTime::DateFormat::Rfc3339, DateTime::TimeFractionFormat::AllDigits)
                  << " T: " << std::this_thread::get_id() << "] " << LogLevelToConsoleString(level)
                  << " : " << message;

        // If the message ends with a new line, flush the stream otherwise insert a new line to
        // terminate the message.
        //
        // If the client of the logger APIs is using the stream form of the logger, then it will
        // insert a \n character when the client uses std::endl. This check ensures that we don't
        // insert unnecessary new lines.
        if (!message.empty() && message.back() == '\n')
        {
          std::cerr << std::flush;
        }
        else
        {
          std::cerr << std::endl;
        }
      };

  return consoleLogger;
}

namespace {
static bool g_initialized;
} // namespace

bool EnvironmentLogLevelListener::IsInitialized() { return g_initialized; }

void EnvironmentLogLevelListener::SetInitialized(bool value) { g_initialized = value; }
