// Tests pre-image truncation after clean / unclean shutdown when pre-images expire by a set
// 'expireAfterSeconds'. For more details about the test implementation, see the definition of
// 'PreImageTruncateAfterShutdownTest'.
//
// @tags: [
//  requires_fcv_72,
//  # Not suitable for inMemory variants given data must persist across shutdowns.
//  requires_persistence,
//  assumes_against_mongod_not_mongos,
//  requires_replication,
// ]

import {
    PreImageTruncateAfterShutdownTest
} from "jstests/noPassthrough/libs/change_stream_pre_image_truncate_after_shutdown.js";

const testName = "pre_image_truncate_after_shutdown_expire_after_seconds";
const preImageTruncateAfterShutdownTest = new PreImageTruncateAfterShutdownTest(testName);
preImageTruncateAfterShutdownTest.setup();

/////////////////////////////////////////////////////////////////////////////////////////////////
//
//      UNCLEAN SHUTDOWNS
//      Only 'numUnexpiredPreImages' should remain post shutdown.
//
////////////////////////////////////////////////////////////////////////

// Test as Primary.
preImageTruncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
    runAsPrimary: true,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: false,
});
// Test as secondary.
preImageTruncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
    runAsPrimary: false,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: false,
});
// No expired pre-images.
preImageTruncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
    runAsPrimary: true,
    numExpiredPreImages: 0,
    numUnexpiredPreImages: 6,
    cleanShutdown: false,
});
// All expired pre-images.
preImageTruncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
    runAsPrimary: true,
    numExpiredPreImages: 6,
    numUnexpiredPreImages: 0,
    cleanShutdown: false,
});

/////////////////////////////////////////////////////////////////////////////////////////////////
//
//      CLEAN SHUTDOWNS
//      All pre-images should exist past shutdown - no immediate truncation is necessary for data
//      consisitency.
//
/////////////////////////////////////////////////////////////////////////////////////////////////
// Test as Primary.
preImageTruncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
    runAsPrimary: true,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: true,
});
// Test as secondary.
preImageTruncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
    runAsPrimary: false,
    numExpiredPreImages: 4,
    numUnexpiredPreImages: 2,
    cleanShutdown: true,
});
// No expired pre-images.
preImageTruncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
    runAsPrimary: true,
    numExpiredPreImages: 0,
    numUnexpiredPreImages: 6,
    cleanShutdown: true,
});
// All expired pre-images.
preImageTruncateAfterShutdownTest.testTruncateByExpireAfterSeconds({
    runAsPrimary: true,
    numExpiredPreImages: 6,
    numUnexpiredPreImages: 0,
    cleanShutdown: true,
});

preImageTruncateAfterShutdownTest.teardown();
