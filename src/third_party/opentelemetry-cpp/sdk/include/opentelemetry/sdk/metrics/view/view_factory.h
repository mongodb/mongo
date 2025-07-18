// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>

#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"
#include "opentelemetry/sdk/metrics/view/view.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

/**
 * Factory class for View.
 */
class OPENTELEMETRY_EXPORT ViewFactory
{
public:
  static std::unique_ptr<View> Create(const std::string &name);

  static std::unique_ptr<View> Create(const std::string &name, const std::string &description);

  static std::unique_ptr<View> Create(const std::string &name,
                                      const std::string &description,
                                      const std::string &unit);

  static std::unique_ptr<View> Create(const std::string &name,
                                      const std::string &description,
                                      const std::string &unit,
                                      AggregationType aggregation_type);

  static std::unique_ptr<View> Create(const std::string &name,
                                      const std::string &description,
                                      const std::string &unit,
                                      AggregationType aggregation_type,
                                      std::shared_ptr<AggregationConfig> aggregation_config);

  static std::unique_ptr<View> Create(const std::string &name,
                                      const std::string &description,
                                      const std::string &unit,
                                      AggregationType aggregation_type,
                                      std::shared_ptr<AggregationConfig> aggregation_config,
                                      std::unique_ptr<AttributesProcessor> attributes_processor);
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
