// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/random_id_generator_factory.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/trace/random_id_generator.h"
#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

std::unique_ptr<IdGenerator> RandomIdGeneratorFactory::Create()
{
  std::unique_ptr<IdGenerator> id_generator(new RandomIdGenerator());
  return id_generator;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
