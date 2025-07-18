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
const size_t kOverflowAttributesHash          = opentelemetry::sdk::common::GetHashForAttributeMap(
    {{kAttributesLimitOverflowKey,
               kAttributesLimitOverflowValue}});  // precalculated for optimization

class AttributeHashGenerator
{
public:
  size_t operator()(const MetricAttributes &attributes) const
  {
    return opentelemetry::sdk::common::GetHashForAttributeMap(attributes);
  }
};

class AttributesHashMap
{
public:
  AttributesHashMap(size_t attributes_limit = kAggregationCardinalityLimit)
      : attributes_limit_(attributes_limit)
  {}
  Aggregation *Get(size_t hash) const
  {
    auto it = hash_map_.find(hash);
    if (it != hash_map_.end())
    {
      return it->second.second.get();
    }
    return nullptr;
  }

  /**
   * @return check if key is present in hash
   *
   */
  bool Has(size_t hash) const { return hash_map_.find(hash) != hash_map_.end(); }

  /**
   * @return the pointer to value for given key if present.
   * If not present, it uses the provided callback to generate
   * value and store in the hash
   */
  Aggregation *GetOrSetDefault(const opentelemetry::common::KeyValueIterable &attributes,
                               const AttributesProcessor *attributes_processor,
                               std::function<std::unique_ptr<Aggregation>()> aggregation_callback,
                               size_t hash)
  {
    auto it = hash_map_.find(hash);
    if (it != hash_map_.end())
    {
      return it->second.second.get();
    }

    if (IsOverflowAttributes())
    {
      return GetOrSetOveflowAttributes(aggregation_callback);
    }

    MetricAttributes attr{attributes, attributes_processor};

    hash_map_[hash] = {attr, aggregation_callback()};
    return hash_map_[hash].second.get();
  }

  Aggregation *GetOrSetDefault(std::function<std::unique_ptr<Aggregation>()> aggregation_callback,
                               size_t hash)
  {
    auto it = hash_map_.find(hash);
    if (it != hash_map_.end())
    {
      return it->second.second.get();
    }

    if (IsOverflowAttributes())
    {
      return GetOrSetOveflowAttributes(aggregation_callback);
    }

    MetricAttributes attr{};
    hash_map_[hash] = {attr, aggregation_callback()};
    return hash_map_[hash].second.get();
  }

  Aggregation *GetOrSetDefault(const MetricAttributes &attributes,
                               std::function<std::unique_ptr<Aggregation>()> aggregation_callback,
                               size_t hash)
  {
    auto it = hash_map_.find(hash);
    if (it != hash_map_.end())
    {
      return it->second.second.get();
    }

    if (IsOverflowAttributes())
    {
      return GetOrSetOveflowAttributes(aggregation_callback);
    }

    MetricAttributes attr{attributes};

    hash_map_[hash] = {attr, aggregation_callback()};
    return hash_map_[hash].second.get();
  }

  /**
   * Set the value for given key, overwriting the value if already present
   */
  void Set(const opentelemetry::common::KeyValueIterable &attributes,
           const AttributesProcessor *attributes_processor,
           std::unique_ptr<Aggregation> aggr,
           size_t hash)
  {
    auto it = hash_map_.find(hash);
    if (it != hash_map_.end())
    {
      it->second.second = std::move(aggr);
    }
    else if (IsOverflowAttributes())
    {
      hash_map_[kOverflowAttributesHash] = {
          MetricAttributes{{kAttributesLimitOverflowKey, kAttributesLimitOverflowValue}},
          std::move(aggr)};
    }
    else
    {
      MetricAttributes attr{attributes, attributes_processor};
      hash_map_[hash] = {attr, std::move(aggr)};
    }
  }

  void Set(const MetricAttributes &attributes, std::unique_ptr<Aggregation> aggr, size_t hash)
  {
    auto it = hash_map_.find(hash);
    if (it != hash_map_.end())
    {
      it->second.second = std::move(aggr);
    }
    else if (IsOverflowAttributes())
    {
      hash_map_[kOverflowAttributesHash] = {
          MetricAttributes{{kAttributesLimitOverflowKey, kAttributesLimitOverflowValue}},
          std::move(aggr)};
    }
    else
    {
      hash_map_[hash] = {attributes, std::move(aggr)};
    }
  }

  /**
   * Iterate the hash to yield key and value stored in hash.
   */
  bool GetAllEnteries(
      nostd::function_ref<bool(const MetricAttributes &, Aggregation &)> callback) const
  {
    for (auto &kv : hash_map_)
    {
      if (!callback(kv.second.first, *(kv.second.second.get())))
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

private:
  std::unordered_map<size_t, std::pair<MetricAttributes, std::unique_ptr<Aggregation>>> hash_map_;
  size_t attributes_limit_;

  Aggregation *GetOrSetOveflowAttributes(
      std::function<std::unique_ptr<Aggregation>()> aggregation_callback)
  {
    auto agg = aggregation_callback();
    return GetOrSetOveflowAttributes(std::move(agg));
  }

  Aggregation *GetOrSetOveflowAttributes(std::unique_ptr<Aggregation> agg)
  {
    auto it = hash_map_.find(kOverflowAttributesHash);
    if (it != hash_map_.end())
    {
      return it->second.second.get();
    }

    MetricAttributes attr{{kAttributesLimitOverflowKey, kAttributesLimitOverflowValue}};
    hash_map_[kOverflowAttributesHash] = {attr, std::move(agg)};
    return hash_map_[kOverflowAttributesHash].second.get();
  }

  bool IsOverflowAttributes() const { return (hash_map_.size() + 1 >= attributes_limit_); }
};
}  // namespace metrics

}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
