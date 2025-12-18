/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace service
{

/**
  The string ID of the service instance.
  <p>
  MUST be unique for each instance of the same @code service.namespace,service.name @endcode pair
  (in other words
  @code service.namespace,service.name,service.instance.id @endcode triplet MUST be globally
  unique). The ID helps to distinguish instances of the same service that exist at the same time
  (e.g. instances of a horizontally scaled service). <p> Implementations, such as SDKs, are
  recommended to generate a random Version 1 or Version 4 <a
  href="https://www.ietf.org/rfc/rfc4122.txt">RFC 4122</a> UUID, but are free to use an inherent
  unique ID as the source of this value if stability is desirable. In that case, the ID SHOULD be
  used as source of a UUID Version 5 and SHOULD use the following UUID as the namespace: @code
  4d63009a-8d0f-11ee-aad7-4c796ed8e320 @endcode. <p> UUIDs are typically recommended, as only an
  opaque value for the purposes of identifying a service instance is needed. Similar to what can be
  seen in the man page for the <a
  href="https://www.freedesktop.org/software/systemd/man/latest/machine-id.html">@code
  /etc/machine-id @endcode</a> file, the underlying data, such as pod name and namespace should be
  treated as confidential, being the user's choice to expose it or not via another resource
  attribute. <p> For applications running behind an application server (like unicorn), we do not
  recommend using one identifier for all processes participating in the application. Instead, it's
  recommended each division (e.g. a worker thread in unicorn) to have its own instance.id. <p> It's
  not recommended for a Collector to set @code service.instance.id @endcode if it can't
  unambiguously determine the service instance that is generating that telemetry. For instance,
  creating an UUID based on @code pod.name @endcode will likely be wrong, as the Collector might not
  know from which container within that pod the telemetry originated. However, Collectors can set
  the @code service.instance.id @endcode if they can unambiguously determine the service instance
  for that telemetry. This is typically the case for scraping receivers, as they know the target
  address and port.
 */
static constexpr const char *kServiceInstanceId = "service.instance.id";

/**
  Logical name of the service.
  <p>
  MUST be the same for all instances of horizontally scaled services. If the value was not
  specified, SDKs MUST fallback to @code unknown_service: @endcode concatenated with <a
  href="process.md">@code process.executable.name @endcode</a>, e.g. @code unknown_service:bash
  @endcode. If @code process.executable.name @endcode is not available, the value MUST be set to
  @code unknown_service @endcode.
 */
static constexpr const char *kServiceName = "service.name";

/**
  A namespace for @code service.name @endcode.
  <p>
  A string value having a meaning that helps to distinguish a group of services, for example the
  team name that owns a group of services. @code service.name @endcode is expected to be unique
  within the same namespace. If @code service.namespace @endcode is not specified in the Resource
  then @code service.name @endcode is expected to be unique for all services that have no explicit
  namespace defined (so the empty/unspecified namespace is simply one more valid namespace).
  Zero-length namespace string is assumed equal to unspecified namespace.
 */
static constexpr const char *kServiceNamespace = "service.namespace";

/**
  The version string of the service API or implementation. The format is not defined by these
  conventions.
 */
static constexpr const char *kServiceVersion = "service.version";

}  // namespace service
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
