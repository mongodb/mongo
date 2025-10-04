// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/trace/random_id_generator_factory.h"
#include "opentelemetry/sdk/trace/random_id_generator.h"
#include "opentelemetry/version.h"

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
