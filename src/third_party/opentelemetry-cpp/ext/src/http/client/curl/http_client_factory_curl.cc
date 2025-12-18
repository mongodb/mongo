// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include "opentelemetry/ext/http/client/curl/http_client_curl.h"
#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/ext/http/client/http_client_factory.h"
#include "opentelemetry/sdk/common/thread_instrumentation.h"

namespace http_client = opentelemetry::ext::http::client;

std::shared_ptr<http_client::HttpClient> http_client::HttpClientFactory::Create()
{
  return std::make_shared<http_client::curl::HttpClient>();
}

std::shared_ptr<http_client::HttpClient> http_client::HttpClientFactory::Create(
    const std::shared_ptr<sdk::common::ThreadInstrumentation> &thread_instrumentation)
{
  return std::make_shared<http_client::curl::HttpClient>(thread_instrumentation);
}

std::shared_ptr<http_client::HttpClientSync> http_client::HttpClientFactory::CreateSync()
{
  return std::make_shared<http_client::curl::HttpClientSync>();
}
