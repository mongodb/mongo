// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/version.h"

#include <chrono>
#include <string>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

struct OtlpGrpcClientOptions
{
  /** The endpoint to export to. */
  std::string endpoint;

  /** Use SSL. */
  bool use_ssl_credentials;

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

  /** Export timeout. */
  std::chrono::system_clock::duration timeout;

  /** Additional HTTP headers. */
  OtlpHeaders metadata;

  /** User agent. */
  std::string user_agent;

  /** max number of threads that can be allocated from this */
  std::size_t max_threads;

  /** Compression type. */
  std::string compression;

#ifdef ENABLE_ASYNC_EXPORT
  // Concurrent requests
  std::size_t max_concurrent_requests;
#endif
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
