// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>

#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/view/predicate_factory.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class InstrumentSelector
{
public:
  InstrumentSelector(opentelemetry::sdk::metrics::InstrumentType instrument_type,
                     const std::string &name,
                     const std::string &units)
      : name_filter_{PredicateFactory::GetPredicate(name, PredicateType::kPattern)},
        unit_filter_{PredicateFactory::GetPredicate(units, PredicateType::kExact)},
        instrument_type_{instrument_type}
  {}

  // Returns name filter predicate. This shouldn't be deleted
  const opentelemetry::sdk::metrics::Predicate *GetNameFilter() const { return name_filter_.get(); }
  // Returns unit filter predicate. This shouldn't be deleted
  const opentelemetry::sdk::metrics::Predicate *GetUnitFilter() const { return unit_filter_.get(); }
  // Returns instrument filter.
  InstrumentType GetInstrumentType() { return instrument_type_; }

private:
  std::unique_ptr<opentelemetry::sdk::metrics::Predicate> name_filter_;
  std::unique_ptr<opentelemetry::sdk::metrics::Predicate> unit_filter_;
  opentelemetry::sdk::metrics::InstrumentType instrument_type_;
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
