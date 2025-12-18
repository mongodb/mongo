// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/version.h"

#include <chrono>
#include <memory>
#include <string>

namespace grpc
{
class ChannelCredentials;
}

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

struct OtlpGrpcClientOptions
{
  virtual ~OtlpGrpcClientOptions()                                = default;
  OtlpGrpcClientOptions()                                         = default;
  OtlpGrpcClientOptions(const OtlpGrpcClientOptions &)            = default;
  OtlpGrpcClientOptions(OtlpGrpcClientOptions &&)                 = default;
  OtlpGrpcClientOptions &operator=(const OtlpGrpcClientOptions &) = default;
  OtlpGrpcClientOptions &operator=(OtlpGrpcClientOptions &&)      = default;

  /** The endpoint to export to. */
  std::string endpoint;

  /** Use SSL. */
  bool use_ssl_credentials{};

  /** CA CERT, path to a file. */
  std::string ssl_credentials_cacert_path;

  /** CA CERT, as a string. */
  std::string ssl_credentials_cacert_as_string;

#ifdef ENABLE_OTLP_GRPC_SSL_MTLS_PREVIEW
  /** CLIENT KEY, path to a file. */
  std::string ssl_client_key_path;

  /** CLIENT KEY, as a string. */
  std::string ssl_client_key_string;

  /** CLIENT CERT, path to a file. */
  std::string ssl_client_cert_path;

  /** CLIENT CERT, as a string. */
  std::string ssl_client_cert_string;
#endif

#ifdef ENABLE_OTLP_GRPC_CREDENTIAL_PREVIEW
  /** Use custom ChannelCredentials, instead of the SSL options above. */
  std::shared_ptr<grpc::ChannelCredentials> credentials;
#endif

  /** Export timeout. */
  std::chrono::system_clock::duration timeout;

  /** Additional HTTP headers. */
  OtlpHeaders metadata;

  /** User agent. */
  std::string user_agent;

  /** max number of threads that can be allocated from this */
  std::size_t max_threads{};

  /** Compression type. */
  std::string compression;

#ifdef ENABLE_ASYNC_EXPORT
  // Concurrent requests
  std::size_t max_concurrent_requests{};
#endif

  /** The maximum number of call attempts, including the original attempt. */
  std::uint32_t retry_policy_max_attempts{};

  /** The initial backoff delay between retry attempts, random between (0, initial_backoff). */
  std::chrono::duration<float> retry_policy_initial_backoff{};

  /** The maximum backoff places an upper limit on exponential backoff growth. */
  std::chrono::duration<float> retry_policy_max_backoff{};

  /** The backoff will be multiplied by this value after each retry attempt. */
  float retry_policy_backoff_multiplier{};
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
