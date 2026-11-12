// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/shared/extension_version.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {

/**
 * Accessors for the API version negotiated between the host and this extension library.
 *
 * Unlike the host, which may be loaded alongside several extensions at several different
 * negotiated versions, an extension library negotiates exactly one version for its entire
 * lifetime: the host calls get_mongodb_extension() once per shared library, and each library
 * carries its own copy of the statically linked SDK. That makes the negotiated version a property
 * of the library as a whole, so SDK code can reach for it here instead of threading it down from
 * whichever handle happens to be in scope.
 *
 * This is deliberately kept independent of HostServicesAPI's static handle. Whether any given C
 * API type is reachable at the negotiated version is a per-type question, answered by
 * c_api_to_min_supported_capability/c_api_to_max_supported_capability, and the negotiated version
 * is an *input* to every one of those decisions -- including the one that decides whether
 * HostServices itself can be installed. It therefore cannot be stored inside any of the objects it
 * is used to make decisions about.
 */

/**
 * Records the version negotiated with the host. Called once from get_mongodb_extension(), before
 * any other SDK state is established, and unconditionally -- that is, without regard for whether
 * any particular API type is supported at that version -- so that the rejection paths can still
 * report what was negotiated.
 *
 * The first call wins and later calls do not change the recorded version. Within a single
 * extension there is only ever one call; a second would mean two extensions are sharing one copy
 * of the SDK's statics, and keeping the first negotiated version is the better of the two
 * available outcomes. A second call is silently ignored.
 */
void setNegotiatedCapabilityVersion(const CapabilityVersion& capabilityVersion);

/**
 * Test-only: overwrites the negotiated version unconditionally.
 *
 * Unit tests simulate many extension loads inside one process, so the first-call-wins contract of
 * setNegotiatedCapabilityVersion -- which exists to catch two extensions sharing one copy of the
 * SDK's statics -- is the wrong shape there. This setter lets each test fixture establish the
 * version its extension-under-test would have negotiated, and lets rejection tests return the
 * library to the zero "not yet negotiated" marker before exercising get_mongodb_extension().
 *
 * Never call this from extension code: production negotiation goes through
 * setNegotiatedCapabilityVersion.
 */
void setNegotiatedCapabilityVersionForTest(const CapabilityVersion& capabilityVersion);

/**
 * Returns the version negotiated with the host.
 *
 * Returns the zero capability if negotiation has not happened yet. Code that runs before
 * get_mongodb_extension() -- the REGISTER_EXTENSION static registrars and
 * get_mongodb_extension_versions() -- has no negotiated version to report, and handing back a
 * plausible-looking default there would claim a capability the host never agreed to.
 */
const CapabilityVersion& negotiatedCapabilityVersion();

}  // namespace mongo::extension::sdk
