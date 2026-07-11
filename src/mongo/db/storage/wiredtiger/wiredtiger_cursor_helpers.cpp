// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/testing_proctor.h"

#include <string>

#include <wiredtiger.h>

namespace mongo {

namespace {
void handleWriteContextForDebugging(WiredTigerRecoveryUnit& ru, WT_CURSOR* cursor) {
    if (ru.shouldGatherWriteContextForDebugging()) {
        BSONObjBuilder builder;

        std::string s;
        StringStackTraceSink sink{s};
        printStackTrace(sink);
        builder.append("stacktrace", s);

        builder.append("uri", cursor->uri);

        ru.storeWriteContextForDebugging(builder.obj());
    }
}
}  // namespace

int wiredTigerCursorInsert(WiredTigerRecoveryUnit& ru, WT_CURSOR* cursor) {
    int ret = cursor->insert(cursor);
    if (MONGO_likely(ret == 0)) {
        ru.setTxnModified();
    }
    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(ru, cursor);
    }
    return ret;
}

int wiredTigerCursorModify(WiredTigerRecoveryUnit& ru,
                           WT_CURSOR* cursor,
                           WT_MODIFY* entries,
                           int nentries) {
    int ret = cursor->modify(cursor, entries, nentries);
    if (MONGO_likely(ret == 0)) {
        ru.setTxnModified();
    }
    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(ru, cursor);
    }
    return ret;
}

int wiredTigerCursorUpdate(WiredTigerRecoveryUnit& ru, WT_CURSOR* cursor) {
    int ret = cursor->update(cursor);
    if (MONGO_likely(ret == 0)) {
        ru.setTxnModified();
    }
    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(ru, cursor);
    }
    return ret;
}

int wiredTigerCursorRemove(WiredTigerRecoveryUnit& ru, WT_CURSOR* cursor) {
    int ret = cursor->remove(cursor);
    if (MONGO_likely(ret == 0)) {
        ru.setTxnModified();
    }
    if (TestingProctor::instance().isEnabled()) {
        handleWriteContextForDebugging(ru, cursor);
    }
    return ret;
}

bool chooseBlindWriteOverwrite(bool defaultOverwrite,
                               bool providerAllowsBlindWrite,
                               PseudoRandom& prng) {
    // Blind writes are a one-way upgrade: false -> (maybe) true. If the caller's default already
    // asks for overwrite=true, preserve it. Downgrading true -> false is never useful (the point
    // of blind writes is to skip a lookup, not add a redundant one) and is actively unsafe for
    // callers whose invariants depend on overwrite=true.
    if (defaultOverwrite) {
        return true;
    }
    if (!providerAllowsBlindWrite) {
        return false;
    }
    const double ratio = gWiredTigerBlindWriteRatio.load();
    if (ratio >= 1.0) {
        return true;
    }
    if (ratio <= 0.0) {
        return false;
    }
    return prng.trueWithProbability(ratio);
}

namespace {
// Set the blind-write ratio to 0 when TestingProctor is enabled so correctness suites exercise
// the non-blind path regardless of the server parameter's default. Runs after TestingDiagnostics
// so the proctor's final state is visible, and unit tests can still override the ratio at runtime.
MONGO_INITIALIZER_WITH_PREREQUISITES(SetBlindWriteRatioUnderTestingProctor, ("TestingDiagnostics"))
(InitializerContext*) {
    if (TestingProctor::instance().isEnabled()) {
        gWiredTigerBlindWriteRatio.store(0.0);
    }
}
}  // namespace

}  // namespace mongo
