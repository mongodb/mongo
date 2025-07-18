// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "opentelemetry/common/attribute_value.h"
#include "opentelemetry/exporters/otlp/otlp_populate_attribute_utils.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/utility.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/version.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
#include "opentelemetry/proto/common/v1/common.pb.h"
#include "opentelemetry/proto/resource/v1/resource.pb.h"
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

namespace nostd = opentelemetry::nostd;

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

//
// See `attribute_value.h` for details.
//
const int kAttributeValueSize      = 16;
const int kOwnedAttributeValueSize = 15;

void OtlpPopulateAttributeUtils::PopulateAnyValue(
    opentelemetry::proto::common::v1::AnyValue *proto_value,
    const opentelemetry::common::AttributeValue &value) noexcept
{
  if (nullptr == proto_value)
  {
    return;
  }

  // Assert size of variant to ensure that this method gets updated if the variant
  // definition changes
  static_assert(
      nostd::variant_size<opentelemetry::common::AttributeValue>::value == kAttributeValueSize,
      "AttributeValue contains unknown type");

  if (nostd::holds_alternative<bool>(value))
  {
    proto_value->set_bool_value(nostd::get<bool>(value));
  }
  else if (nostd::holds_alternative<int>(value))
  {
    proto_value->set_int_value(nostd::get<int>(value));
  }
  else if (nostd::holds_alternative<int64_t>(value))
  {
    proto_value->set_int_value(nostd::get<int64_t>(value));
  }
  else if (nostd::holds_alternative<unsigned int>(value))
  {
    proto_value->set_int_value(nostd::get<unsigned int>(value));
  }
  else if (nostd::holds_alternative<uint64_t>(value))
  {
    proto_value->set_int_value(
        nostd::get<uint64_t>(value));  // NOLINT(cppcoreguidelines-narrowing-conversions)
  }
  else if (nostd::holds_alternative<double>(value))
  {
    proto_value->set_double_value(nostd::get<double>(value));
  }
  else if (nostd::holds_alternative<const char *>(value))
  {
    proto_value->set_string_value(nostd::get<const char *>(value));
  }
  else if (nostd::holds_alternative<nostd::string_view>(value))
  {
    proto_value->set_string_value(nostd::get<nostd::string_view>(value).data(),
                                  nostd::get<nostd::string_view>(value).size());
  }
  else if (nostd::holds_alternative<nostd::span<const uint8_t>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<nostd::span<const uint8_t>>(value))
    {
      array_value->add_values()->set_int_value(val);
    }
  }
  else if (nostd::holds_alternative<nostd::span<const bool>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<nostd::span<const bool>>(value))
    {
      array_value->add_values()->set_bool_value(val);
    }
  }
  else if (nostd::holds_alternative<nostd::span<const int>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<nostd::span<const int>>(value))
    {
      array_value->add_values()->set_int_value(val);
    }
  }
  else if (nostd::holds_alternative<nostd::span<const int64_t>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<nostd::span<const int64_t>>(value))
    {
      array_value->add_values()->set_int_value(val);
    }
  }
  else if (nostd::holds_alternative<nostd::span<const unsigned int>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<nostd::span<const unsigned int>>(value))
    {
      array_value->add_values()->set_int_value(val);
    }
  }
  else if (nostd::holds_alternative<nostd::span<const uint64_t>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<nostd::span<const uint64_t>>(value))
    {
      array_value->add_values()->set_int_value(
          val);  // NOLINT(cppcoreguidelines-narrowing-conversions)
    }
  }
  else if (nostd::holds_alternative<nostd::span<const double>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<nostd::span<const double>>(value))
    {
      array_value->add_values()->set_double_value(val);
    }
  }
  else if (nostd::holds_alternative<nostd::span<const nostd::string_view>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<nostd::span<const nostd::string_view>>(value))
    {
      array_value->add_values()->set_string_value(val.data(), val.size());
    }
  }
}

void OtlpPopulateAttributeUtils::PopulateAnyValue(
    opentelemetry::proto::common::v1::AnyValue *proto_value,
    const opentelemetry::sdk::common::OwnedAttributeValue &value) noexcept
{
  if (nullptr == proto_value)
  {
    return;
  }

  // Assert size of variant to ensure that this method gets updated if the variant
  // definition changes
  static_assert(nostd::variant_size<opentelemetry::sdk::common::OwnedAttributeValue>::value ==
                    kOwnedAttributeValueSize,
                "OwnedAttributeValue contains unknown type");

  if (nostd::holds_alternative<bool>(value))
  {
    proto_value->set_bool_value(nostd::get<bool>(value));
  }
  else if (nostd::holds_alternative<int32_t>(value))
  {
    proto_value->set_int_value(nostd::get<int32_t>(value));
  }
  else if (nostd::holds_alternative<int64_t>(value))
  {
    proto_value->set_int_value(nostd::get<int64_t>(value));
  }
  else if (nostd::holds_alternative<uint32_t>(value))
  {
    proto_value->set_int_value(nostd::get<uint32_t>(value));
  }
  else if (nostd::holds_alternative<uint64_t>(value))
  {
    proto_value->set_int_value(
        nostd::get<uint64_t>(value));  // NOLINT(cppcoreguidelines-narrowing-conversions)
  }
  else if (nostd::holds_alternative<double>(value))
  {
    proto_value->set_double_value(nostd::get<double>(value));
  }
  else if (nostd::holds_alternative<std::string>(value))
  {
    proto_value->set_string_value(nostd::get<std::string>(value));
  }
  else if (nostd::holds_alternative<std::vector<bool>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto val : nostd::get<std::vector<bool>>(value))
    {
      array_value->add_values()->set_bool_value(val);
    }
  }
  else if (nostd::holds_alternative<std::vector<int32_t>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<std::vector<int32_t>>(value))
    {
      array_value->add_values()->set_int_value(val);
    }
  }
  else if (nostd::holds_alternative<std::vector<uint32_t>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<std::vector<uint32_t>>(value))
    {
      array_value->add_values()->set_int_value(
          val);  // NOLINT(cppcoreguidelines-narrowing-conversions)
    }
  }
  else if (nostd::holds_alternative<std::vector<int64_t>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<std::vector<int64_t>>(value))
    {
      array_value->add_values()->set_int_value(val);
    }
  }
  else if (nostd::holds_alternative<std::vector<uint64_t>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<std::vector<uint64_t>>(value))
    {
      array_value->add_values()->set_int_value(
          val);  // NOLINT(cppcoreguidelines-narrowing-conversions)
    }
  }
  else if (nostd::holds_alternative<std::vector<double>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<std::vector<double>>(value))
    {
      array_value->add_values()->set_double_value(val);
    }
  }
  else if (nostd::holds_alternative<std::vector<std::string>>(value))
  {
    auto array_value = proto_value->mutable_array_value();
    for (const auto &val : nostd::get<std::vector<std::string>>(value))
    {
      array_value->add_values()->set_string_value(val);
    }
  }
}

void OtlpPopulateAttributeUtils::PopulateAttribute(
    opentelemetry::proto::common::v1::KeyValue *attribute,
    nostd::string_view key,
    const opentelemetry::common::AttributeValue &value) noexcept
{
  if (nullptr == attribute)
  {
    return;
  }

  // Assert size of variant to ensure that this method gets updated if the variant
  // definition changes
  static_assert(
      nostd::variant_size<opentelemetry::common::AttributeValue>::value == kAttributeValueSize,
      "AttributeValue contains unknown type");

  attribute->set_key(key.data(), key.size());
  PopulateAnyValue(attribute->mutable_value(), value);
}

/** Maps from C++ attribute into OTLP proto attribute. */
void OtlpPopulateAttributeUtils::PopulateAttribute(
    opentelemetry::proto::common::v1::KeyValue *attribute,
    nostd::string_view key,
    const opentelemetry::sdk::common::OwnedAttributeValue &value) noexcept
{
  if (nullptr == attribute)
  {
    return;
  }

  // Assert size of variant to ensure that this method gets updated if the variant
  // definition changes
  static_assert(nostd::variant_size<opentelemetry::sdk::common::OwnedAttributeValue>::value ==
                    kOwnedAttributeValueSize,
                "OwnedAttributeValue contains unknown type");

  attribute->set_key(key.data(), key.size());
  PopulateAnyValue(attribute->mutable_value(), value);
}

void OtlpPopulateAttributeUtils::PopulateAttribute(
    opentelemetry::proto::resource::v1::Resource *proto,
    const opentelemetry::sdk::resource::Resource &resource) noexcept
{
  if (nullptr == proto)
  {
    return;
  }

  for (const auto &kv : resource.GetAttributes())
  {
    OtlpPopulateAttributeUtils::PopulateAttribute(proto->add_attributes(), kv.first, kv.second);
  }
}

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
