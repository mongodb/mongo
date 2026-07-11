// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/testing_options_gen.h"
#include "mongo/util/testing_proctor.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

/**
 * The following initializer must always run before "DisableTestingDiagnosticsByDefault" to ensure
 * it is allowed to set (enables/disables) testing diagnostics.
 */
MONGO_INITIALIZER_GENERAL(TestingDiagnostics,
                          ("EndStartupOptionStorage"),
                          ("DisableTestingDiagnosticsByDefault"))
(InitializerContext*) {
    // Initialize testing diagnostics only if it has not been already initialized, or it must be
    // enabled by the initializer (i.e., "testingDiagnosticsEnabled=true"). This ensures testing
    // diagnostics cannot be set beyond this point.
    if (!TestingProctor::instance().isInitialized() || gTestingDiagnosticsEnabledAtStartup) {
        TestingProctor::instance().setEnabled(gTestingDiagnosticsEnabledAtStartup);
    }

    if (TestingProctor::instance().isEnabled()) {
        LOGV2_OPTIONS(4672602,
                      {logv2::LogTag::kStartupWarnings},
                      "Testing behaviors are enabled. This has serious implications for both "
                      "performance and security.");
    }
}

}  // namespace
}  // namespace mongo
