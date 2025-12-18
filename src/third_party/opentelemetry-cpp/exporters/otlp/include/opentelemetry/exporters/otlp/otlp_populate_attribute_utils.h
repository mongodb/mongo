// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/common/attribute_value.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/version.h"

namespace opentelemetry
{
namespace proto
{

namespace common
{
namespace v1
{
class AnyValue;
class KeyValue;
class InstrumentationScope;
}  // namespace v1
}  // namespace common

namespace resource
{
namespace v1
{
class Resource;
}
}  // namespace resource

}  // namespace proto
}  // namespace opentelemetry

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{
/**
 * The OtlpCommoneUtils contains utility functions to populate attributes
 */
class OtlpPopulateAttributeUtils
{

public:
  static void PopulateAttribute(opentelemetry::proto::resource::v1::Resource *proto,
                                const opentelemetry::sdk::resource::Resource &resource) noexcept;

  static void PopulateAttribute(opentelemetry::proto::common::v1::InstrumentationScope *proto,
                                const opentelemetry::sdk::instrumentationscope::InstrumentationScope
                                    &instrumentation_scope) noexcept;

  static void PopulateAnyValue(opentelemetry::proto::common::v1::AnyValue *proto_value,
                               const opentelemetry::common::AttributeValue &value,
                               bool allow_bytes) noexcept;

  static void PopulateAnyValue(opentelemetry::proto::common::v1::AnyValue *proto_value,
                               const opentelemetry::sdk::common::OwnedAttributeValue &value,
                               bool allow_bytes) noexcept;

  static void PopulateAttribute(opentelemetry::proto::common::v1::KeyValue *attribute,
                                nostd::string_view key,
                                const opentelemetry::common::AttributeValue &value,
                                bool allow_bytes) noexcept;

  static void PopulateAttribute(opentelemetry::proto::common::v1::KeyValue *attribute,
                                nostd::string_view key,
                                const opentelemetry::sdk::common::OwnedAttributeValue &value,
                                bool allow_bytes) noexcept;
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
