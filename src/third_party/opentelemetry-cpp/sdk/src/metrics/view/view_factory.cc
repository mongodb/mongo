// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <utility>

#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"
#include "opentelemetry/sdk/metrics/view/view.h"
#include "opentelemetry/sdk/metrics/view/view_factory.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

std::unique_ptr<View> ViewFactory::Create(const std::string &name)
{
  return Create(name, "");
}

std::unique_ptr<View> ViewFactory::Create(const std::string &name, const std::string &description)
{
  return Create(name, description, "", AggregationType::kDefault);
}

std::unique_ptr<View> ViewFactory::Create(const std::string &name,
                                          const std::string &description,
                                          const std::string &unit)
{
  return Create(name, description, unit, AggregationType::kDefault);
}

std::unique_ptr<View> ViewFactory::Create(const std::string &name,
                                          const std::string &description,
                                          const std::string &unit,
                                          AggregationType aggregation_type)
{
  std::shared_ptr<AggregationConfig> aggregation_config(nullptr);
  return Create(name, description, unit, aggregation_type, aggregation_config);
}

std::unique_ptr<View> ViewFactory::Create(const std::string &name,
                                          const std::string &description,
                                          const std::string &unit,
                                          AggregationType aggregation_type,
                                          std::shared_ptr<AggregationConfig> aggregation_config)
{
  auto attributes_processor =
      std::unique_ptr<AttributesProcessor>(new DefaultAttributesProcessor());

  return Create(name, description, unit, aggregation_type, std::move(aggregation_config),
                std::move(attributes_processor));
}

std::unique_ptr<View> ViewFactory::Create(const std::string &name,
                                          const std::string &description,
                                          const std::string &unit,
                                          AggregationType aggregation_type,
                                          std::shared_ptr<AggregationConfig> aggregation_config,
                                          std::unique_ptr<AttributesProcessor> attributes_processor)
{
  std::unique_ptr<View> view(new View(name, description, unit, aggregation_type,
                                      std::move(aggregation_config),
                                      std::move(attributes_processor)));
  return view;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
