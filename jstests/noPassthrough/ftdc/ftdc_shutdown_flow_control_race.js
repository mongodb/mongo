/**
 * SERVER-126496: Repro for TSAN data race between FTDC's periodic serverStatus collector and
 * FlowControl (and DisaggFlowControlShim) singleton teardown during shutdown.
 *
 * Race shape:
 *   T1 (FTDC controller thread): every diagnosticDataCollectionPeriodMillis, calls every
 *      ServerStatusSection::generateSection, including FlowControlServerStatusSection, which
 *      calls FlowControl::get() and reads the ServiceContext decoration slot.
 *   T2 (shutdown thread):  mongod_main shutdown sequence calls FlowControl::shutdown() (which
 *      does globalFlow.reset()) BEFORE stopMongoDFTDC(). The gap between these two points is
 *      the race window. The disagg shim has the same shape: its destructor runs during
 *      serviceLifecycle.shutdownStateRequiredForStorageAccess(), which also precedes
 *      stopMongoDFTDC().
 *
 * Strategy: widen the race window with the existing injectFTDCServerStatusCollectionDelay
 * failpoint so an FTDC collection is mid-flight when shutdown starts. Without a fix the
 * shutdown thread races the in-flight section read; under TSAN this produces a data race
 * report on the decoration slot. With the fix (the singleton teardown either moves after
 * stopMongoDFTDC or coordinates with the FTDC collector) the test should run clean.
 *
 * @tags: [
 *   requires_fcv_82,
 *   featureFlagGaplessFTDC,
 *   # Race only manifests with a real storage engine + replication subsystem present so the
 *   # FlowControl singleton is constructed and the relevant shutdown ordering runs.
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Short collection period so FTDC's controller thread is actively polling serverStatus while
// shutdown runs. The disable-on-shutdown failpoint guarantees at least one collection is
// in-flight (paused inside generateSection) when we issue the shutdown command.
const kCollectionPeriodMs = 100;
const kCollectionTimeoutMs = 5000;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            featureFlagGaplessFTDC: true,
            diagnosticDataCollectionPeriodMillis: kCollectionPeriodMs,
            diagnosticDataCollectionSampleTimeoutMillis: kCollectionTimeoutMs,
            // Single-threaded FTDC collector keeps the race deterministic: exactly one in-flight
            // reader of the FlowControl decoration slot at the moment we tear it down.
            diagnosticDataCollectionMinThreads: 1,
            diagnosticDataCollectionMaxThreads: 1,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB("admin");

// Confirm FTDC is actually collecting flowControl before we begin: this section is the read
// site for FlowControl::get() that races with FlowControl::shutdown().
const ftdcData = verifyGetDiagnosticData(adminDb);
assert(ftdcData.hasOwnProperty("serverStatus"),
       "expected serverStatus in FTDC: " + tojson(ftdcData));
assert(ftdcData["serverStatus"].hasOwnProperty("flowControl"),
       "expected flowControl section in FTDC serverStatus (this is the racing read site): " +
           tojson(ftdcData["serverStatus"]));

// Arm the failpoint that pauses inside the FTDC serverStatus collection path. This widens the
// race window so that when shutdown begins, the FTDC collector thread is parked inside
// generateSection -> FlowControl::get() reading the decoration slot that shutdown is about to
// reset(). pauseWhileSet ensures the collector is suspended *inside* the read.
const fp = configureFailPoint(primary, "injectFTDCServerStatusCollectionDelay");
fp.waitWithTimeout(kCollectionPeriodMs * 10);

// Kick off shutdown asynchronously: it will progress past FlowControl::shutdown() and
// serviceLifecycle.shutdownStateRequiredForStorageAccess() (which destroys the disagg shim) on
// its way to stopMongoDFTDC(). With the bug, the racing write happens while the FTDC thread is
// still parked at the failpoint reading the slot.
const shutdownAwait = startParallelShell(function() {
    // adminCommand({shutdown: 1}) returns a network error on success because the connection
    // closes; assert.soon retries until the node is gone.
    assert.soon(() => {
        try {
            db.getSiblingDB("admin").runCommand({shutdown: 1, force: true});
            return false;  // still up; retry
        } catch (e) {
            return true;  // socket closed -> shutdown ran
        }
    }, "mongod did not shut down", 60 * 1000);
}, primary.port);

// Give the shutdown thread time to enter FlowControl::shutdown() / disagg shim destructor.
// The failpoint is still latched: the FTDC reader is parked inside the section, so the writes
// happen with no synchronisation on the decoration slot. Releasing the failpoint then lets
// the reader run to completion on (with the bug) an object that has just been reset()'d.
sleep(2000);
fp.off();

shutdownAwait();

// Stop the rest of the set cleanly. Under TSAN, any data race observed by the runtime during
// the shutdown above is reported on the test's stderr and surfaces as a non-zero exit; that
// is what discriminates "buggy" from "fixed" — there is no JS-visible assertion to make.
rst.stopSet(/*signal=*/ undefined, /*forRestart=*/ false, {skipValidation: true});
