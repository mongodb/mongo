// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstddef>

#include "opentelemetry/exporters/otlp/otlp_file_client_options.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/version.h"

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
 * The OTLP File client exports data in OpenTelemetry Protocol (OTLP) format.
 */
class OtlpFileClient
{
public:
  /**
   * Create an OtlpFileClient using the given options.
   */
  explicit OtlpFileClient(OtlpFileClientOptions &&options);

  ~OtlpFileClient();

  /**
   * Sync export
   * @param message message to export, it should be ExportTraceServiceRequest,
   * ExportMetricsServiceRequest or ExportLogsServiceRequest
   * @param record_count record count of the message
   * @return return the status of this operation
   */
  sdk::common::ExportResult Export(const google::protobuf::Message &message,
                                   std::size_t record_count) noexcept;

  /**
   * Force flush the file client.
   */
  bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  /**
   * Shut down the file client.
   * @param timeout an optional timeout, the default timeout of 0 means that no
   * timeout is applied.
   * @return return the status of this operation
   */
  bool Shutdown(std::chrono::microseconds timeout = std::chrono::microseconds(0)) noexcept;

  /**
   * Get options of current OTLP file client.
   * @return options of current OTLP file client.
   */
  inline const OtlpFileClientOptions &GetOptions() const noexcept { return options_; }

  /**
   * Get if this OTLP file client is shutdown.
   * @return return true after Shutdown is called.
   */
  bool IsShutdown() const noexcept;

private:
  // Stores if this file client had its Shutdown() method called
  bool is_shutdown_;

  // The configuration options associated with this file client.
  const OtlpFileClientOptions options_;

  opentelemetry::nostd::shared_ptr<OtlpFileAppender> backend_;
};
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
