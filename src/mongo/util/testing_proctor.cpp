/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


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
