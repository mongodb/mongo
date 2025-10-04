// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <unordered_map>
#include <utility>

#include "opentelemetry/common/attribute_value.h"
#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/metrics/state/filtered_ordered_attribute_map.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
using MetricAttributes = opentelemetry::sdk::metrics::FilteredOrderedAttributeMap;

/**
 * The AttributesProcessor is responsible for customizing which
 * attribute(s) are to be reported as metrics dimension(s).
 */

class AttributesProcessor
{
public:
  // Process the metric instrument attributes.
  // @returns integer with individual bits set if they are to be filtered.

  virtual MetricAttributes process(
      const opentelemetry::common::KeyValueIterable &attributes) const noexcept = 0;

  virtual bool isPresent(nostd::string_view key) const noexcept = 0;

  virtual ~AttributesProcessor() = default;
};

/**
 * DefaultAttributesProcessor returns copy of input instrument attributes without
 * any modification.
 */

class DefaultAttributesProcessor : public AttributesProcessor
{
public:
  MetricAttributes process(
      const opentelemetry::common::KeyValueIterable &attributes) const noexcept override
  {
    MetricAttributes result(attributes);
    return result;
  }

  bool isPresent(nostd::string_view /*key*/) const noexcept override { return true; }
};

/**
 * FilteringAttributesProcessor  filters by allowed attribute names and drops any names
 * that are not in the allow list.
 */

class FilteringAttributesProcessor : public AttributesProcessor
{
public:
  FilteringAttributesProcessor(
      const std::unordered_map<std::string, bool> allowed_attribute_keys = {})
      : allowed_attribute_keys_(std::move(allowed_attribute_keys))
  {}

  MetricAttributes process(
      const opentelemetry::common::KeyValueIterable &attributes) const noexcept override
  {
    MetricAttributes result;
    attributes.ForEachKeyValue(
        [&](nostd::string_view key, opentelemetry::common::AttributeValue value) noexcept {
          if (allowed_attribute_keys_.find(key.data()) != allowed_attribute_keys_.end())
          {
            result.SetAttribute(key, value);
            return true;
          }
          return true;
        });
    return result;
  }

  bool isPresent(nostd::string_view key) const noexcept override
  {
    return (allowed_attribute_keys_.find(key.data()) != allowed_attribute_keys_.end());
  }

private:
  std::unordered_map<std::string, bool> allowed_attribute_keys_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
