// Tests time-based pre-image retention policy of change stream pre-images remover job.
// @tags: [
//  requires_replication,
// ]
(function() {
"use strict";

load("jstests/noPassthrough/libs/change_stream_pre_image_time_based_expiration_utils.js");

// Tests pre-image time based expiration on a replica-set.
(function testChangeStreamPreImagesforTimeBasedExpirationOnReplicaSet() {
    const replSetTest = new ReplSetTest({name: "replSet", nodes: 3});
    replSetTest.startSet();
    replSetTest.initiate();

    const conn = replSetTest.getPrimary();
    const primary = replSetTest.getPrimary();
    testTimeBasedPreImageRetentionPolicy(conn, primary);
    replSetTest.stopSet();
})();
}());
