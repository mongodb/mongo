// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/sdk/common/attributemap_hash.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation.h"
#include "opentelemetry/sdk/metrics/state/filtered_ordered_attribute_map.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

using opentelemetry::sdk::common::OrderedAttributeMap;

constexpr size_t kAggregationCardinalityLimit = 2000;
const std::string kAttributesLimitOverflowKey = "otel.metrics.overflow";
const bool kAttributesLimitOverflowValue      = true;
const MetricAttributes kOverflowAttributes    = {
    {kAttributesLimitOverflowKey,
        kAttributesLimitOverflowValue}};  // precalculated for optimization

class AttributeHashGenerator
{
public:
  size_t operator()(const MetricAttributes &attributes) const
  {
    return opentelemetry::sdk::common::GetHashForAttributeMap(attributes);
  }
};

template <typename CustomHash = MetricAttributesHash>
class AttributesHashMapWithCustomHash
{
public:
  AttributesHashMapWithCustomHash(size_t attributes_limit = kAggregationCardinalityLimit)
      : attributes_limit_(attributes_limit)
  {
    if (attributes_limit_ > kAggregationCardinalityLimit)
    {
      hash_map_.reserve(attributes_limit_);
    }
  }

  Aggregation *Get(const MetricAttributes &attributes) const
  {
    auto it = hash_map_.find(attributes);
    if (it != hash_map_.end())
    {
      return it->second.get();
    }
    return nullptr;
  }

  /**
   * @return check if key is present in hash
   *
   */
  bool Has(const MetricAttributes &attributes) const
  {
    return hash_map_.find(attributes) != hash_map_.end();
  }

  /**
   * @return the pointer to value for given key if present.
   * If not present, it uses the provided callback to generate
   * value and store in the hash
   */
  Aggregation *GetOrSetDefault(
      const opentelemetry::common::KeyValueIterable &attributes,
      const AttributesProcessor *attributes_processor,
      nostd::function_ref<std::unique_ptr<Aggregation>()> aggregation_callback)
  {
    // TODO: avoid constructing MetricAttributes from KeyValueIterable for
    // hash_map_.find which is a heavy operation
    MetricAttributes attr{attributes, attributes_processor};

    auto it = hash_map_.find(attr);
    if (it != hash_map_.end())
    {
      return it->second.get();
    }

    if (IsOverflowAttributes(attr))
    {
      return GetOrSetOveflowAttributes(aggregation_callback);
    }

    auto result = hash_map_.emplace(std::move(attr), aggregation_callback());
    return result.first->second.get();
  }

  Aggregation *GetOrSetDefault(
      const MetricAttributes &attributes,
      nostd::function_ref<std::unique_ptr<Aggregation>()> aggregation_callback)
  {
    auto it = hash_map_.find(attributes);
    if (it != hash_map_.end())
    {
      return it->second.get();
    }

    if (IsOverflowAttributes(attributes))
    {
      return GetOrSetOveflowAttributes(aggregation_callback);
    }

    hash_map_[attributes] = aggregation_callback();
    return hash_map_[attributes].get();
  }

  Aggregation *GetOrSetDefault(
      MetricAttributes &&attributes,
      nostd::function_ref<std::unique_ptr<Aggregation>()> aggregation_callback)
  {
    auto it = hash_map_.find(attributes);
    if (it != hash_map_.end())
    {
      return it->second.get();
    }

    if (IsOverflowAttributes(attributes))
    {
      return GetOrSetOveflowAttributes(aggregation_callback);
    }

    auto result = hash_map_.emplace(std::move(attributes), aggregation_callback());
    return result.first->second.get();
  }
  /**
   * Set the value for given key, overwriting the value if already present
   */
  void Set(const opentelemetry::common::KeyValueIterable &attributes,
           const AttributesProcessor *attributes_processor,
           std::unique_ptr<Aggregation> aggr)
  {
    Set(MetricAttributes{attributes, attributes_processor}, std::move(aggr));
  }

  void Set(const MetricAttributes &attributes, std::unique_ptr<Aggregation> aggr)
  {
    auto it = hash_map_.find(attributes);
    if (it != hash_map_.end())
    {
      it->second = std::move(aggr);
    }
    else if (IsOverflowAttributes(attributes))
    {
      hash_map_[kOverflowAttributes] = std::move(aggr);
    }
    else
    {
      hash_map_[attributes] = std::move(aggr);
    }
  }

  void Set(MetricAttributes &&attributes, std::unique_ptr<Aggregation> aggr)
  {
    auto it = hash_map_.find(attributes);
    if (it != hash_map_.end())
    {
      it->second = std::move(aggr);
    }
    else if (IsOverflowAttributes(attributes))
    {
      hash_map_[kOverflowAttributes] = std::move(aggr);
    }
    else
    {
      hash_map_[std::move(attributes)] = std::move(aggr);
    }
  }

  /**
   * Iterate the hash to yield key and value stored in hash.
   */
  bool GetAllEntries(
      nostd::function_ref<bool(const MetricAttributes &, Aggregation &)> callback) const
  {
    for (auto &kv : hash_map_)
    {
      if (!callback(kv.first, *(kv.second.get())))
      {
        return false;  // callback is not prepared to consume data
      }
    }
    return true;
  }

  /**
   * Return the size of hash.
   */
  size_t Size() { return hash_map_.size(); }

#ifdef UNIT_TESTING
  size_t BucketCount() { return hash_map_.bucket_count(); }
  size_t BucketSize(size_t n) { return hash_map_.bucket_size(n); }
#endif

private:
  std::unordered_map<MetricAttributes, std::unique_ptr<Aggregation>, CustomHash> hash_map_;
  size_t attributes_limit_;

  Aggregation *GetOrSetOveflowAttributes(
      nostd::function_ref<std::unique_ptr<Aggregation>()> aggregation_callback)
  {
    auto agg = aggregation_callback();
    return GetOrSetOveflowAttributes(std::move(agg));
  }

  Aggregation *GetOrSetOveflowAttributes(std::unique_ptr<Aggregation> agg)
  {
    auto it = hash_map_.find(kOverflowAttributes);
    if (it != hash_map_.end())
    {
      return it->second.get();
    }

    auto result = hash_map_.emplace(kOverflowAttributes, std::move(agg));
    return result.first->second.get();
  }

  bool IsOverflowAttributes(const MetricAttributes &attributes) const
  {
    // If the incoming attributes are exactly the overflow sentinel, route
    // directly to the overflow entry.
    if (attributes == kOverflowAttributes)
    {
      return true;
    }
    // Determine if overflow entry already exists.
    bool has_overflow = (hash_map_.find(kOverflowAttributes) != hash_map_.end());
    // If overflow already present, total size already includes it; trigger overflow
    // when current size (including overflow) is >= limit.
    if (has_overflow)
    {
      return hash_map_.size() >= attributes_limit_;
    }
    // If overflow not present yet, simulate adding a new distinct key. If that
    // would exceed the limit, we redirect to overflow instead of creating a
    // new real attribute entry.
    return (hash_map_.size() + 1) >= attributes_limit_;
  }
};

using AttributesHashMap = AttributesHashMapWithCustomHash<>;

}  // namespace metrics

}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
