// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "opentelemetry/ext/http/client/http_client.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace ext
{
namespace http
{
namespace client
{
class HttpClientFactory
{
public:
  static std::shared_ptr<HttpClientSync> CreateSync();

  static std::shared_ptr<HttpClient> Create();
};
}  // namespace client
}  // namespace http
}  // namespace ext
OPENTELEMETRY_END_NAMESPACE
