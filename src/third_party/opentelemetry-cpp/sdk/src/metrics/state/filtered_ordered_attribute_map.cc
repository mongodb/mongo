// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/metrics/state/filtered_ordered_attribute_map.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
FilteredOrderedAttributeMap::FilteredOrderedAttributeMap(
    const opentelemetry::common::KeyValueIterable &attributes,
    const AttributesProcessor *processor)
    : OrderedAttributeMap()
{
  attributes.ForEachKeyValue(
      [&](nostd::string_view key, opentelemetry::common::AttributeValue value) noexcept {
        if (!processor || processor->isPresent(key))
        {
          SetAttribute(key, value);
        }
        return true;
      });

  UpdateHash();
}

FilteredOrderedAttributeMap::FilteredOrderedAttributeMap(
    std::initializer_list<std::pair<nostd::string_view, opentelemetry::common::AttributeValue>>
        attributes,
    const AttributesProcessor *processor)
    : OrderedAttributeMap()
{
  for (auto &kv : attributes)
  {
    if (!processor || processor->isPresent(kv.first))
    {
      SetAttribute(kv.first, kv.second);
    }
  }

  UpdateHash();
}
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
