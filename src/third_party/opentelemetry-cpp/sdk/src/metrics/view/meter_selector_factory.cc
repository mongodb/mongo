// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/metrics/view/meter_selector_factory.h"
#include "opentelemetry/sdk/metrics/view/meter_selector.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

std::unique_ptr<MeterSelector> MeterSelectorFactory::Create(const std::string &name,
                                                            const std::string &version,
                                                            const std::string &schema)
{
  std::unique_ptr<MeterSelector> meter_selector(new MeterSelector(name, version, schema));
  return meter_selector;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
