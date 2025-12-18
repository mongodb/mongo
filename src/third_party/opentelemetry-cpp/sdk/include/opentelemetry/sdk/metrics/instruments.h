// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

#include "opentelemetry/sdk/metrics/state/filtered_ordered_attribute_map.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
enum class InstrumentType
{
  kCounter,
  kHistogram,
  kUpDownCounter,
  kObservableCounter,
  kObservableGauge,
  kObservableUpDownCounter,
  kGauge
};

enum class InstrumentClass
{
  kSync,
  kAsync
};

enum class InstrumentValueType
{
  kInt,
  kLong,
  kFloat,
  kDouble
};

enum class AggregationType
{
  kDrop,
  kHistogram,
  kLastValue,
  kSum,
  kDefault,
  kBase2ExponentialHistogram
};

enum class AggregationTemporality
{
  kUnspecified,
  kDelta,
  kCumulative
};

struct InstrumentDescriptor
{
  std::string name_;
  std::string description_;
  std::string unit_;
  InstrumentType type_;
  InstrumentValueType value_type_;
};

struct InstrumentDescriptorUtil
{
  // Case-insensitive comparison of two ASCII strings used to evaluate equality of instrument names
  static bool CaseInsensitiveAsciiEquals(const std::string &lhs, const std::string &rhs) noexcept
  {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
             return std::tolower(static_cast<unsigned char>(a)) ==
                    std::tolower(static_cast<unsigned char>(b));
           });
  }

  // Implementation of the specification requirements on duplicate instruments
  // An instrument is a duplicate if it has the same name (case-insensitive) as another instrument,
  // but different instrument kind, unit, or description.
  // https://github.com/open-telemetry/opentelemetry-specification/blob/9c8c30631b0e288de93df7452f91ed47f6fba330/specification/metrics/sdk.md?plain=1#L869
  static bool IsDuplicate(const InstrumentDescriptor &lhs, const InstrumentDescriptor &rhs) noexcept
  {
    // Not a duplicate if case-insensitive names are not equal
    if (!InstrumentDescriptorUtil::CaseInsensitiveAsciiEquals(lhs.name_, rhs.name_))
    {
      return false;
    }

    // Duplicate if names equal and kinds (Type and ValueType) are not equal
    if (lhs.type_ != rhs.type_ || lhs.value_type_ != rhs.value_type_)
    {
      return true;
    }

    // Duplicate if names equal and units (case-sensitive) are not equal
    if (lhs.unit_ != rhs.unit_)
    {
      return true;
    }

    // Duplicate if names equal and descriptions (case-sensitive) are not equal
    if (lhs.description_ != rhs.description_)
    {
      return true;
    }

    // All identifying fields are equal
    // These are identical instruments or only have a name case conflict
    return false;
  }

  static opentelemetry::nostd::string_view GetInstrumentTypeString(InstrumentType type) noexcept
  {
    switch (type)
    {
      case InstrumentType::kCounter:
        return "Counter";
      case InstrumentType::kUpDownCounter:
        return "UpDownCounter";
      case InstrumentType::kHistogram:
        return "Histogram";
      case InstrumentType::kObservableCounter:
        return "ObservableCounter";
      case InstrumentType::kObservableUpDownCounter:
        return "ObservableUpDownCounter";
      case InstrumentType::kObservableGauge:
        return "ObservableGauge";
      case InstrumentType::kGauge:
        return "Gauge";
      default:
        return "Unknown";
    }
  }

  static opentelemetry::nostd::string_view GetInstrumentValueTypeString(
      InstrumentValueType value_type) noexcept
  {
    switch (value_type)
    {
      case InstrumentValueType::kInt:
        return "Int";
      case InstrumentValueType::kLong:
        return "Long";
      case InstrumentValueType::kFloat:
        return "Float";
      case InstrumentValueType::kDouble:
        return "Double";
      default:
        return "Unknown";
    }
  }
};

struct InstrumentEqualNameCaseInsensitive
{
  bool operator()(const InstrumentDescriptor &lhs, const InstrumentDescriptor &rhs) const noexcept
  {
    // Names (case-insensitive)
    if (!InstrumentDescriptorUtil::CaseInsensitiveAsciiEquals(lhs.name_, rhs.name_))
    {
      return false;
    }

    // Kinds (Type and ValueType)
    if (lhs.type_ != rhs.type_ || lhs.value_type_ != rhs.value_type_)
    {
      return false;
    }

    // Units (case-sensitive)
    if (lhs.unit_ != rhs.unit_)
    {
      return false;
    }

    // Descriptions (case-sensitive)
    if (lhs.description_ != rhs.description_)
    {
      return false;
    }

    // All identifying fields are equal
    return true;
  }
};

// Hash for InstrumentDescriptor
// Identical instruments must have the same hash value
// Two instruments are identical when all identifying fields (case-insensitive name , kind,
// description, unit) are equal.
struct InstrumentDescriptorHash
{
  std::size_t operator()(const InstrumentDescriptor &instrument_descriptor) const noexcept
  {
    std::size_t hashcode{};

    for (char c : instrument_descriptor.name_)
    {
      sdk::common::GetHash(hashcode,
                           static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    sdk::common::GetHash(hashcode, instrument_descriptor.description_);
    sdk::common::GetHash(hashcode, instrument_descriptor.unit_);
    sdk::common::GetHash(hashcode, static_cast<uint32_t>(instrument_descriptor.type_));
    sdk::common::GetHash(hashcode, static_cast<uint32_t>(instrument_descriptor.value_type_));
    return hashcode;
  }
};

using MetricAttributes               = opentelemetry::sdk::metrics::FilteredOrderedAttributeMap;
using MetricAttributesHash           = opentelemetry::sdk::metrics::FilteredOrderedAttributeMapHash;
using AggregationTemporalitySelector = std::function<AggregationTemporality(InstrumentType)>;

/*class InstrumentSelector {
public:
InstrumentSelector(opentelemetry::nostd::string_view name,
opentelemetry::sdk::metrics::InstrumentType type): name_(name.data()), type_(type) {} InstrumentType
GetType(){return type_;} std::string GetNameFilter() { return name_;}

private:
std::string name_;
InstrumentType type_;
};*/
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
