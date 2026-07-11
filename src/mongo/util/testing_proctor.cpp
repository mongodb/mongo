// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/testing_proctor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <new>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

void TestingProctor::setEnabled(bool enable) {
    if (!isInitialized()) {
        _diagnosticsEnabled = enable;
        return;
    }

    uassert(ErrorCodes::AlreadyInitialized,
            "Cannot alter testing diagnostics once initialized",
            _diagnosticsEnabled.value() == enable);

    LOGV2(4672601, "Overriding testing diagnostics", "enabled"_attr = enable);
}

void TestingProctor::exitAbruptlyIfDeferredErrors(bool verbose) const {
    if (isInitialized() && isEnabled() && haveTripwireAssertionsOccurred()) {
        if (verbose) {
            warnIfTripwireAssertionsOccurred();
        }
        LOGV2_FATAL_NOTRACE(
            4457001, "Aborting process during exit due to prior failed tripwire assertions.");
    }
}

namespace {

/**
 * The initializer ensures that testing diagnostics is always initialized (by default to disabled),
 * especially for those executables that never call into `setEnabled()` (e.g., the mongo shell).
 */
MONGO_INITIALIZER(DisableTestingDiagnosticsByDefault)(InitializerContext*) {
    if (!TestingProctor::instance().isInitialized()) {
        TestingProctor::instance().setEnabled(false);
    }
}

}  // namespace
}  // namespace mongo
