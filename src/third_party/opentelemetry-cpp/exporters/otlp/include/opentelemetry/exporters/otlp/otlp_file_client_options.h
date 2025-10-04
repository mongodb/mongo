// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/variant.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <ostream>
#include <string>

// forward declare google::protobuf::Message
namespace google
{
namespace protobuf
{
class Message;
}
}  // namespace google

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

/**
 * Struct to hold OTLP File client options for file system backend.
 * @note Available placeholder for file_pattern and alias_pattern:
 *     %Y:  writes year as a 4 digit decimal number
 *     %y:  writes last 2 digits of year as a decimal number (range [00,99])
 *     %m:  writes month as a decimal number (range [01,12])
 *     %j:  writes day of the year as a decimal number (range [001,366])
 *     %d:  writes day of the month as a decimal number (range [01,31])
 *     %w:  writes weekday as a decimal number, where Sunday is 0 (range [0-6])
 *     %H:  writes hour as a decimal number, 24 hour clock (range [00-23])
 *     %I:  writes hour as a decimal number, 12 hour clock (range [01,12])
 *     %M:  writes minute as a decimal number (range [00,59])
 *     %S:  writes second as a decimal number (range [00,60])
 *     %F:  equivalent to "%Y-%m-%d" (the ISO 8601 date format)
 *     %T:  equivalent to "%H:%M:%S" (the ISO 8601 time format)
 *     %R:  equivalent to "%H:%M"
 *     %N:  rotate index, start from 0
 *     %n:  rotate index, start from 1
 */
struct OtlpFileClientFileSystemOptions
{
  // Pattern to create output file
  std::string file_pattern;

  // Pattern to create alias file path for the latest file rotation.
  std::string alias_pattern;

  // Flush interval
  std::chrono::microseconds flush_interval = std::chrono::microseconds(30000000);

  // Flush record count
  std::size_t flush_count = 256;

  // Maximum file size
  std::size_t file_size = 1024 * 1024 * 20;

  // Maximum file count
  std::size_t rotate_size = 3;

  inline OtlpFileClientFileSystemOptions() noexcept {}
};

/**
 * Class to append data of OTLP format.
 */
class OtlpFileAppender
{
public:
  virtual ~OtlpFileAppender() = default;

  virtual void Export(opentelemetry::nostd::string_view data, std::size_t record_count) = 0;

  virtual bool ForceFlush(std::chrono::microseconds timeout) noexcept = 0;

  virtual bool Shutdown(std::chrono::microseconds timeout) noexcept = 0;
};

using OtlpFileClientBackendOptions =
    nostd::variant<OtlpFileClientFileSystemOptions,
                   std::reference_wrapper<std::ostream>,
                   opentelemetry::nostd::shared_ptr<OtlpFileAppender>>;

/**
 * Struct to hold OTLP FILE client options.
 */
struct OtlpFileClientOptions
{
  // Whether to print the status of the FILE client in the console
  bool console_debug = false;

  OtlpFileClientBackendOptions backend_options;

  inline OtlpFileClientOptions() noexcept {}
};
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
