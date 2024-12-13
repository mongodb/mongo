// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace resource
{

using ResourceAttributes = opentelemetry::sdk::common::AttributeMap;

class Resource
{
public:
  Resource(const Resource &) = default;

  const ResourceAttributes &GetAttributes() const noexcept;
  const std::string &GetSchemaURL() const noexcept;

  /**
   * Returns a new, merged {@link Resource} by merging the current Resource
   * with the other Resource. In case of a collision, the other Resource takes
   * precedence.
   *
   * The specification notes that if schema urls collide, the resulting
   * schema url is implementation-defined. In the C++ implementation, the
   * schema url of @param other is picked.
   *
   * @param other the Resource that will be merged with this.
   * @returns the newly merged Resource.
   */

  Resource Merge(const Resource &other) const noexcept;

  /**
   * Returns a newly created Resource with the specified attributes.
   * It adds (merge) SDK attributes and OTEL attributes before returning.
   * @param attributes for this resource
   * @returns the newly created Resource.
   */

  static Resource Create(const ResourceAttributes &attributes,
                         const std::string &schema_url = std::string{});

  /**
   * Returns an Empty resource.
   */

  static Resource &GetEmpty();

  /**
   * Returns a Resource that indentifies the SDK in use.
   */

  static Resource &GetDefault();

protected:
  /**
   * The constructor is protected and only for use internally by the class and
   * inside ResourceDetector class.
   * Users should use the Create factory method to obtain a Resource
   * instance.
   */
  Resource(const ResourceAttributes &attributes = ResourceAttributes(),
           const std::string &schema_url        = std::string{}) noexcept;

private:
  ResourceAttributes attributes_;
  std::string schema_url_;

  friend class OTELResourceDetector;
};

}  // namespace resource
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
