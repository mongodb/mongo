// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/common/global_log_handler.h"

#include <iostream>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
namespace internal_log
{

namespace
{
struct GlobalLogHandlerData
{
  nostd::shared_ptr<LogHandler> handler;
  LogLevel log_level{LogLevel::Warning};

  GlobalLogHandlerData() : handler(nostd::shared_ptr<LogHandler>(new DefaultLogHandler)) {}
  ~GlobalLogHandlerData() { is_singleton_destroyed = true; }

  GlobalLogHandlerData(const GlobalLogHandlerData &) = delete;
  GlobalLogHandlerData(GlobalLogHandlerData &&)      = delete;

  GlobalLogHandlerData &operator=(const GlobalLogHandlerData &) = delete;
  GlobalLogHandlerData &operator=(GlobalLogHandlerData &&)      = delete;

  static GlobalLogHandlerData &Instance() noexcept;
  static bool is_singleton_destroyed;
};

bool GlobalLogHandlerData::is_singleton_destroyed = false;

GlobalLogHandlerData &GlobalLogHandlerData::Instance() noexcept
{
  static GlobalLogHandlerData instance;
  return instance;
}

}  // namespace

LogHandler::~LogHandler() {}

void DefaultLogHandler::Handle(LogLevel level,
                               const char *file,
                               int line,
                               const char *msg,
                               const sdk::common::AttributeMap & /* attributes */) noexcept
{
  std::stringstream output_s;
  output_s << "[" << LevelToString(level) << "] ";
  if (file != nullptr)
  {
    output_s << "File: " << file << ":" << line << " ";
  }
  if (msg != nullptr)
  {
    output_s << msg;
  }
  output_s << '\n';
  // TBD - print attributes

  switch (level)
  {
    case LogLevel::Error:
    case LogLevel::Warning:
      std::cerr << output_s.str();  // thread safe.
      break;
    case LogLevel::Info:
    case LogLevel::Debug:
      std::cout << output_s.str();  // thread safe.
      break;
    case LogLevel::None:
    default:
      break;
  }
}

void NoopLogHandler::Handle(LogLevel,
                            const char *,
                            int,
                            const char *,
                            const sdk::common::AttributeMap &) noexcept
{}

nostd::shared_ptr<LogHandler> GlobalLogHandler::GetLogHandler() noexcept
{
  if OPENTELEMETRY_UNLIKELY_CONDITION (GlobalLogHandlerData::is_singleton_destroyed)
  {
    return nostd::shared_ptr<LogHandler>();
  }

  return GlobalLogHandlerData::Instance().handler;
}

void GlobalLogHandler::SetLogHandler(const nostd::shared_ptr<LogHandler> &eh) noexcept
{
  if OPENTELEMETRY_UNLIKELY_CONDITION (GlobalLogHandlerData::is_singleton_destroyed)
  {
    return;
  }

  GlobalLogHandlerData::Instance().handler = eh;
}

LogLevel GlobalLogHandler::GetLogLevel() noexcept
{
  if OPENTELEMETRY_UNLIKELY_CONDITION (GlobalLogHandlerData::is_singleton_destroyed)
  {
    return LogLevel::None;
  }

  return GlobalLogHandlerData::Instance().log_level;
}

void GlobalLogHandler::SetLogLevel(LogLevel level) noexcept
{
  if OPENTELEMETRY_UNLIKELY_CONDITION (GlobalLogHandlerData::is_singleton_destroyed)
  {
    return;
  }
  GlobalLogHandlerData::Instance().log_level = level;
}

}  // namespace internal_log
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
