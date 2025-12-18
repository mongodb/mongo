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
namespace peer
{

/**
  The <a href="/docs/resource/README.md#service">@code service.name @endcode</a> of the remote
  service. SHOULD be equal to the actual @code service.name @endcode resource attribute of the
  remote service if any. <p> Examples of @code peer.service @endcode that users may specify: <ul>
    <li>A Redis cache of auth tokens as @code peer.service="AuthTokenCache" @endcode.</li>
    <li>A gRPC service @code rpc.service="io.opentelemetry.AuthService" @endcode may be hosted in
  both a gateway, @code peer.service="ExternalApiService" @endcode and a backend, @code
  peer.service="AuthService" @endcode.</li>
  </ul>
 */
static constexpr const char *kPeerService = "peer.service";

}  // namespace peer
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
