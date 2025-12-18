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
namespace cloudfoundry
{

/**
  The guid of the application.
  <p>
  Application instrumentation should use the value from environment
  variable @code VCAP_APPLICATION.application_id @endcode. This is the same value as
  reported by @code cf app <app-name> --guid @endcode.
 */
static constexpr const char *kCloudfoundryAppId = "cloudfoundry.app.id";

/**
  The index of the application instance. 0 when just one instance is active.
  <p>
  CloudFoundry defines the @code instance_id @endcode in the <a
  href="https://github.com/cloudfoundry/loggregator-api#v2-envelope">Loggregator v2 envelope</a>. It
  is used for logs and metrics emitted by CloudFoundry. It is supposed to contain the application
  instance index for applications deployed on the runtime. <p> Application instrumentation should
  use the value from environment variable @code CF_INSTANCE_INDEX @endcode.
 */
static constexpr const char *kCloudfoundryAppInstanceId = "cloudfoundry.app.instance.id";

/**
  The name of the application.
  <p>
  Application instrumentation should use the value from environment
  variable @code VCAP_APPLICATION.application_name @endcode. This is the same value
  as reported by @code cf apps @endcode.
 */
static constexpr const char *kCloudfoundryAppName = "cloudfoundry.app.name";

/**
  The guid of the CloudFoundry org the application is running in.
  <p>
  Application instrumentation should use the value from environment
  variable @code VCAP_APPLICATION.org_id @endcode. This is the same value as
  reported by @code cf org <org-name> --guid @endcode.
 */
static constexpr const char *kCloudfoundryOrgId = "cloudfoundry.org.id";

/**
  The name of the CloudFoundry organization the app is running in.
  <p>
  Application instrumentation should use the value from environment
  variable @code VCAP_APPLICATION.org_name @endcode. This is the same value as
  reported by @code cf orgs @endcode.
 */
static constexpr const char *kCloudfoundryOrgName = "cloudfoundry.org.name";

/**
  The UID identifying the process.
  <p>
  Application instrumentation should use the value from environment
  variable @code VCAP_APPLICATION.process_id @endcode. It is supposed to be equal to
  @code VCAP_APPLICATION.app_id @endcode for applications deployed to the runtime.
  For system components, this could be the actual PID.
 */
static constexpr const char *kCloudfoundryProcessId = "cloudfoundry.process.id";

/**
  The type of process.
  <p>
  CloudFoundry applications can consist of multiple jobs. Usually the
  main process will be of type @code web @endcode. There can be additional background
  tasks or side-cars with different process types.
 */
static constexpr const char *kCloudfoundryProcessType = "cloudfoundry.process.type";

/**
  The guid of the CloudFoundry space the application is running in.
  <p>
  Application instrumentation should use the value from environment
  variable @code VCAP_APPLICATION.space_id @endcode. This is the same value as
  reported by @code cf space <space-name> --guid @endcode.
 */
static constexpr const char *kCloudfoundrySpaceId = "cloudfoundry.space.id";

/**
  The name of the CloudFoundry space the application is running in.
  <p>
  Application instrumentation should use the value from environment
  variable @code VCAP_APPLICATION.space_name @endcode. This is the same value as
  reported by @code cf spaces @endcode.
 */
static constexpr const char *kCloudfoundrySpaceName = "cloudfoundry.space.name";

/**
  A guid or another name describing the event source.
  <p>
  CloudFoundry defines the @code source_id @endcode in the <a
  href="https://github.com/cloudfoundry/loggregator-api#v2-envelope">Loggregator v2 envelope</a>. It
  is used for logs and metrics emitted by CloudFoundry. It is supposed to contain the component
  name, e.g. "gorouter", for CloudFoundry components. <p> When system components are instrumented,
  values from the <a href="https://bosh.io/docs/jobs/#properties-spec">Bosh spec</a> should be used.
  The @code system.id @endcode should be set to
  @code spec.deployment/spec.name @endcode.
 */
static constexpr const char *kCloudfoundrySystemId = "cloudfoundry.system.id";

/**
  A guid describing the concrete instance of the event source.
  <p>
  CloudFoundry defines the @code instance_id @endcode in the <a
  href="https://github.com/cloudfoundry/loggregator-api#v2-envelope">Loggregator v2 envelope</a>. It
  is used for logs and metrics emitted by CloudFoundry. It is supposed to contain the vm id for
  CloudFoundry components. <p> When system components are instrumented, values from the <a
  href="https://bosh.io/docs/jobs/#properties-spec">Bosh spec</a> should be used. The @code
  system.instance.id @endcode should be set to @code spec.id @endcode.
 */
static constexpr const char *kCloudfoundrySystemInstanceId = "cloudfoundry.system.instance.id";

}  // namespace cloudfoundry
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
