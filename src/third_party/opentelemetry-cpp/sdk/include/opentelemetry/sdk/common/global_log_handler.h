// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sstream>  // IWYU pragma: keep
#include <string>
#include <utility>

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/version.h"

#define OTEL_INTERNAL_LOG_LEVEL_NONE 0
#define OTEL_INTERNAL_LOG_LEVEL_ERROR 1
#define OTEL_INTERNAL_LOG_LEVEL_WARN 2
#define OTEL_INTERNAL_LOG_LEVEL_INFO 3
#define OTEL_INTERNAL_LOG_LEVEL_DEBUG 4
#ifndef OTEL_INTERNAL_LOG_LEVEL
// DEBUG by default, we can change log level on runtime
#  define OTEL_INTERNAL_LOG_LEVEL OTEL_INTERNAL_LOG_LEVEL_DEBUG
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
namespace internal_log
{

enum class LogLevel
{
  None    = OTEL_INTERNAL_LOG_LEVEL_NONE,
  Error   = OTEL_INTERNAL_LOG_LEVEL_ERROR,
  Warning = OTEL_INTERNAL_LOG_LEVEL_WARN,
  Info    = OTEL_INTERNAL_LOG_LEVEL_INFO,
  Debug   = OTEL_INTERNAL_LOG_LEVEL_DEBUG
};

inline std::string LevelToString(LogLevel level)
{
  switch (level)
  {
    case LogLevel::None:
      return "None";
    case LogLevel::Error:
      return "Error";
    case LogLevel::Warning:
      return "Warning";
    case LogLevel::Info:
      return "Info";
    case LogLevel::Debug:
      return "Debug";
  }
  return {};
}

class LogHandler
{
public:
  virtual ~LogHandler();

  virtual void Handle(LogLevel level,
                      const char *file,
                      int line,
                      const char *msg,
                      const opentelemetry::sdk::common::AttributeMap &attributes) noexcept = 0;
};

class DefaultLogHandler : public LogHandler
{
public:
  void Handle(LogLevel level,
              const char *file,
              int line,
              const char *msg,
              const opentelemetry::sdk::common::AttributeMap &attributes) noexcept override;
};

class NoopLogHandler : public LogHandler
{
public:
  void Handle(LogLevel level,
              const char *file,
              int line,
              const char *msg,
              const opentelemetry::sdk::common::AttributeMap &error_attributes) noexcept override;
};

/**
 * Stores the singleton global LogHandler.
 */
class GlobalLogHandler
{
public:
  /**
   * Returns the singleton LogHandler.
   *
   * By default, a default LogHandler is returned.
   */
  static inline const nostd::shared_ptr<LogHandler> &GetLogHandler() noexcept
  {
    return GetHandlerAndLevel().first;
  }

  /**
   * Changes the singleton LogHandler.
   * This should be called once at the start of application before creating any Provider
   * instance.
   */
  static inline void SetLogHandler(nostd::shared_ptr<LogHandler> eh) noexcept
  {
    GetHandlerAndLevel().first = eh;
  }

  /**
   * Returns the singleton log level.
   *
   * By default, a default log level is returned.
   */
  static inline LogLevel GetLogLevel() noexcept { return GetHandlerAndLevel().second; }

  /**
   * Changes the singleton Log level.
   * This should be called once at the start of application before creating any Provider
   * instance.
   */
  static inline void SetLogLevel(LogLevel level) noexcept { GetHandlerAndLevel().second = level; }

private:
  static std::pair<nostd::shared_ptr<LogHandler>, LogLevel> &GetHandlerAndLevel() noexcept;
};

}  // namespace internal_log
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

/**
 * GlobalLogHandler and TracerProvider/MeterProvider/LoggerProvider are lazy singletons.
 * To ensure that GlobalLogHandler is the first one to be initialized (and so last to be
 * destroyed), it is first used inside the constructors of TraceProvider, MeterProvider
 * and LoggerProvider for debug logging. */
#define OTEL_INTERNAL_LOG_DISPATCH(level, message, attributes)                            \
  do                                                                                      \
  {                                                                                       \
    using opentelemetry::sdk::common::internal_log::GlobalLogHandler;                     \
    using opentelemetry::sdk::common::internal_log::LogHandler;                           \
    if (level > GlobalLogHandler::GetLogLevel())                                          \
    {                                                                                     \
      break;                                                                              \
    }                                                                                     \
    const opentelemetry::nostd::shared_ptr<LogHandler> &log_handler =                     \
        GlobalLogHandler::GetLogHandler();                                                \
    if (!log_handler)                                                                     \
    {                                                                                     \
      break;                                                                              \
    }                                                                                     \
    std::stringstream tmp_stream;                                                         \
    tmp_stream << message;                                                                \
    log_handler->Handle(level, __FILE__, __LINE__, tmp_stream.str().c_str(), attributes); \
  } while (false);

#define OTEL_INTERNAL_LOG_GET_3RD_ARG(arg1, arg2, arg3, ...) arg3

#if OTEL_INTERNAL_LOG_LEVEL >= OTEL_INTERNAL_LOG_LEVEL_ERROR
#  define OTEL_INTERNAL_LOG_ERROR_1_ARGS(message)                                                  \
    OTEL_INTERNAL_LOG_DISPATCH(opentelemetry::sdk::common::internal_log::LogLevel::Error, message, \
                               {})
#  define OTEL_INTERNAL_LOG_ERROR_2_ARGS(message, attributes)                                      \
    OTEL_INTERNAL_LOG_DISPATCH(opentelemetry::sdk::common::internal_log::LogLevel::Error, message, \
                               attributes)
#  define OTEL_INTERNAL_LOG_ERROR_MACRO(...)                                   \
    OTEL_INTERNAL_LOG_GET_3RD_ARG(__VA_ARGS__, OTEL_INTERNAL_LOG_ERROR_2_ARGS, \
                                  OTEL_INTERNAL_LOG_ERROR_1_ARGS)
#  define OTEL_INTERNAL_LOG_ERROR(...) OTEL_INTERNAL_LOG_ERROR_MACRO(__VA_ARGS__)(__VA_ARGS__)
#else
#  define OTEL_INTERNAL_LOG_ERROR(...)
#endif

#if OTEL_INTERNAL_LOG_LEVEL >= OTEL_INTERNAL_LOG_LEVEL_WARN
#  define OTEL_INTERNAL_LOG_WARN_1_ARGS(message)                                            \
    OTEL_INTERNAL_LOG_DISPATCH(opentelemetry::sdk::common::internal_log::LogLevel::Warning, \
                               message, {})
#  define OTEL_INTERNAL_LOG_WARN_2_ARGS(message, attributes)                                \
    OTEL_INTERNAL_LOG_DISPATCH(opentelemetry::sdk::common::internal_log::LogLevel::Warning, \
                               message, attributes)
#  define OTEL_INTERNAL_LOG_WARN_MACRO(...)                                   \
    OTEL_INTERNAL_LOG_GET_3RD_ARG(__VA_ARGS__, OTEL_INTERNAL_LOG_WARN_2_ARGS, \
                                  OTEL_INTERNAL_LOG_WARN_1_ARGS)
#  define OTEL_INTERNAL_LOG_WARN(...) OTEL_INTERNAL_LOG_WARN_MACRO(__VA_ARGS__)(__VA_ARGS__)
#else
#  define OTEL_INTERNAL_LOG_WARN(...)
#endif

#if OTEL_INTERNAL_LOG_LEVEL >= OTEL_INTERNAL_LOG_LEVEL_DEBUG
#  define OTEL_INTERNAL_LOG_DEBUG_1_ARGS(message)                                                  \
    OTEL_INTERNAL_LOG_DISPATCH(opentelemetry::sdk::common::internal_log::LogLevel::Debug, message, \
                               {})
#  define OTEL_INTERNAL_LOG_DEBUG_2_ARGS(message, attributes)                                      \
    OTEL_INTERNAL_LOG_DISPATCH(opentelemetry::sdk::common::internal_log::LogLevel::Debug, message, \
                               attributes)
#  define OTEL_INTERNAL_LOG_DEBUG_MACRO(...)                                   \
    OTEL_INTERNAL_LOG_GET_3RD_ARG(__VA_ARGS__, OTEL_INTERNAL_LOG_DEBUG_2_ARGS, \
                                  OTEL_INTERNAL_LOG_DEBUG_1_ARGS)
#  define OTEL_INTERNAL_LOG_DEBUG(...) OTEL_INTERNAL_LOG_DEBUG_MACRO(__VA_ARGS__)(__VA_ARGS__)
#else
#  define OTEL_INTERNAL_LOG_DEBUG(...)
#endif

#if OTEL_INTERNAL_LOG_LEVEL >= OTEL_INTERNAL_LOG_LEVEL_INFO
#  define OTEL_INTERNAL_LOG_INFO_1_ARGS(message)                                                  \
    OTEL_INTERNAL_LOG_DISPATCH(opentelemetry::sdk::common::internal_log::LogLevel::Info, message, \
                               {})
#  define OTEL_INTERNAL_LOG_INFO_2_ARGS(message, attributes)                                      \
    OTEL_INTERNAL_LOG_DISPATCH(opentelemetry::sdk::common::internal_log::LogLevel::Info, message, \
                               attributes)
#  define OTEL_INTERNAL_LOG_INFO_MACRO(...)                                   \
    OTEL_INTERNAL_LOG_GET_3RD_ARG(__VA_ARGS__, OTEL_INTERNAL_LOG_INFO_2_ARGS, \
                                  OTEL_INTERNAL_LOG_INFO_1_ARGS)
#  define OTEL_INTERNAL_LOG_INFO(...) OTEL_INTERNAL_LOG_INFO_MACRO(__VA_ARGS__)(__VA_ARGS__)
#else
#  define OTEL_INTERNAL_LOG_INFO(...)
#endif
