// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>

#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
/**
 * View defines the interface to allow SDK user to
 * customize the metrics before exported.
 */

class View
{
public:
  View(const std::string &name,
       const std::string &description                        = "",
       const std::string &unit                               = "",
       AggregationType aggregation_type                      = AggregationType::kDefault,
       std::shared_ptr<AggregationConfig> aggregation_config = nullptr,
       std::unique_ptr<opentelemetry::sdk::metrics::AttributesProcessor> attributes_processor =
           std::unique_ptr<opentelemetry::sdk::metrics::AttributesProcessor>(
               new opentelemetry::sdk::metrics::DefaultAttributesProcessor()))
      : name_(name),
        description_(description),
        unit_(unit),
        aggregation_type_{aggregation_type},
        aggregation_config_{aggregation_config},
        attributes_processor_{std::move(attributes_processor)}
  {}

  virtual ~View() = default;

  virtual std::string GetName() const noexcept { return name_; }

  virtual std::string GetDescription() const noexcept { return description_; }

  virtual AggregationType GetAggregationType() const noexcept { return aggregation_type_; }

  virtual AggregationConfig *GetAggregationConfig() const noexcept
  {
    return aggregation_config_.get();
  }

  virtual const opentelemetry::sdk::metrics::AttributesProcessor &GetAttributesProcessor()
      const noexcept
  {
    return *attributes_processor_.get();
  }

private:
  std::string name_;
  std::string description_;
  std::string unit_;
  AggregationType aggregation_type_;
  std::shared_ptr<AggregationConfig> aggregation_config_;
  std::unique_ptr<opentelemetry::sdk::metrics::AttributesProcessor> attributes_processor_;
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
