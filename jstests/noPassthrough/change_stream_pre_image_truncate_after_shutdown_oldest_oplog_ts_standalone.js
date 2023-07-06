// Tests pre-image truncation after clean / unclean shutdown when pre-images expire by the oldest
// oplog entry. In the tests, a node is both shutdown as a replica set member, its pre-image
// contents are validated as standalone, then the node is rejoined with the replica set.  For more
// details about the test implementation, see the definition of 'PreImageTruncateAfterShutdownTest'.
//
// @tags: [
//  featureFlagUseUnreplicatedTruncatesForDeletions,
//  assumes_against_mongod_not_mongos,
//  requires_replication,
// ]

import {
    PreImageTruncateAfterShutdownTest
} from "jstests/noPassthrough/libs/change_stream_pre_image_truncate_after_shutdown.js";

const testName = "pre_image_truncate_after_shutdown_oldest_oplog_ts_standalone";
const preImageTruncateAfterShutdownTest = new PreImageTruncateAfterShutdownTest(testName);
preImageTruncateAfterShutdownTest.setup();

/////////////////////////////////////////////////////////////////////////////////////////////////
//
//      UNCLEAN SHUTDOWNS
//      Only 'numUnexpiredPreImages' should remain post shutdown.
//
////////////////////////////////////////////////////////////////////////

// Test as Primary.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: true,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: false,
    restartWithQueryableBackup: false,
    restartWithRecoverToOplogTimestamp: false,
    restartWithRecoverFromOplogAsStandalone: true,
});

// Test as secondary.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: false,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: false,
    restartWithQueryableBackup: false,
    restartWithRecoverToOplogTimestamp: false,
    restartWithRecoverFromOplogAsStandalone: true,
});

// Test with all options false.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: false,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: false,
    restartWithQueryableBackup: false,
    restartWithRecoverToOplogTimestamp: false,
    restartWithRecoverFromOplogAsStandalone: false,
});

// Test with 'queryableBackupMode'.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: true,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: false,
    restartWithQueryableBackup: true,
    restartWithRecoverToOplogTimestamp: false,
    restartWithRecoverFromOplogAsStandalone: false,
});
// Test with 'queryableBackupMode' and 'recoverFromOplogTimestamp'.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: true,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: false,
    restartWithQueryableBackup: true,
    restartWithRecoverToOplogTimestamp: true,
    restartWithRecoverFromOplogAsStandalone: false,
});
// Test with no expired pre images.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: true,
    numExpiredPreImages: 0,
    numUnexpiredPreImages: 6,
    cleanShutdown: false,
    restartWithQueryableBackup: false,
    restartWithRecoverToOplogTimestamp: false,
    restartWithRecoverFromOplogAsStandalone: true,
});

/////////////////////////////////////////////////////////////////////////////////////////////////
//
//      CLEAN SHUTDOWNS
//      All pre-images should exist past shutdown - no immediate truncation is necessary for data
//      consisitency.
//
/////////////////////////////////////////////////////////////////////////////////////////////////
// Test as Primary.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: true,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: true,
    restartWithQueryableBackup: false,
    restartWithRecoverToOplogTimestamp: false,
    restartWithRecoverFromOplogAsStandalone: true,
});
// Test as secondary.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: false,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: true,
    restartWithQueryableBackup: false,
    restartWithRecoverToOplogTimestamp: false,
    restartWithRecoverFromOplogAsStandalone: true,
});
// Test with 'queryableBackupMode'.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: true,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: true,
    restartWithQueryableBackup: true,
    restartWithRecoverToOplogTimestamp: false,
    restartWithRecoverFromOplogAsStandalone: false,
});
// Test with 'queryableBackupMode' and 'recoverFromOplogTimestamp'.
preImageTruncateAfterShutdownTest.testTruncateByOldestOplogTSStandalone({
    runAsPrimary: true,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: true,
    restartWithQueryableBackup: true,
    restartWithRecoverToOplogTimestamp: true,
    restartWithRecoverFromOplogAsStandalone: false,
});

preImageTruncateAfterShutdownTest.teardown();
