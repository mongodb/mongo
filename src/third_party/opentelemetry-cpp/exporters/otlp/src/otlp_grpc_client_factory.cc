// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/otlp/otlp_grpc_client_factory.h"
#include <memory>

#include "opentelemetry/exporters/otlp/otlp_grpc_client.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

std::shared_ptr<OtlpGrpcClient> OtlpGrpcClientFactory::Create(const OtlpGrpcClientOptions &options)
{
  return std::make_shared<OtlpGrpcClient>(options);
}

std::shared_ptr<OtlpGrpcClientReferenceGuard> OtlpGrpcClientFactory::CreateReferenceGuard()
{
  return std::make_shared<OtlpGrpcClientReferenceGuard>();
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
