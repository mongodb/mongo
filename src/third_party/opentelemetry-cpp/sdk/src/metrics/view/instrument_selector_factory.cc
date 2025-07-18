// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/metrics/view/instrument_selector_factory.h"
#include "opentelemetry/sdk/metrics/view/instrument_selector.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

std::unique_ptr<InstrumentSelector> InstrumentSelectorFactory::Create(
    opentelemetry::sdk::metrics::InstrumentType instrument_type,
    const std::string &name,
    const std::string &unit)
{
  std::unique_ptr<InstrumentSelector> instrument_selector(
      new InstrumentSelector(instrument_type, name, unit));
  return instrument_selector;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
