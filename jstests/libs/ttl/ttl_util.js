/**
 * Utilities for testing TTL collections.
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

export const TTLUtil = class {
    /**
     * Wait until documents inserted before a call to this function have been visited by a TTL
     * monitor pass. On replica sets, by default the function waits for the TTL deletes to become
     * visible with read concern 'majority'.
     *
     * @param {DB} db   Database connection.
     * @param {boolean} waitForMajorityCommit   Only applies when 'db' is from a replica set, set to
     *     false to disable waiting for TTL deletes to become majority commited.
     * @param {number} timeoutMillis   timeout in milliseconds for the TTL wait. Defaults to
     *     whatever value is used for assert.soon.
     */
    static waitForPass(db, waitForMajorityCommit = true, timeoutMillis = undefined) {
        // The 'ttl.passes' metric is incremented when the TTL monitor has finished a pass.
        // Depending on the timing of the pass, seeing an increment of this metric might not
        // necessarily imply the data we are expecting to be deleted has been seen, as the TTL pass
        // might have been in progress while the data was inserted. Waiting to see two increases of
        // this metric guarantees that the TTL has started a new pass after test data insertion.
        const ttlPasses = db.serverStatus().metrics.ttl.passes;
        assert.soon(
            function () {
                return db.serverStatus().metrics.ttl.passes > ttlPasses + 1;
            },
            "Expected 2 TTL passes but achieved less than 2 in the given time",
            timeoutMillis,
        );

        // Readers using a "majority" read concern might expect TTL deletes to be visible after
        // waitForPass. TTL writes do not imply 'majority' nor 'j: true', and are made durable by
        // the journal flusher when a flush cycle happens every 'commitIntervalMs'. Even in single
        // node replica sets, depending on journal flush timing, it is possible that TTL deletes
        // have not been made durable after returning from this function, and are not considered
        // majority commited. We force the majority commit point to include the TTL writes up to
        // this point in time.
        if (FixtureHelpers.isReplSet(db) && waitForMajorityCommit) {
            // waitForMajorityCommit will never be true if 'db' is not part of a replica set.
            FixtureHelpers.awaitLastOpCommitted(db);
        }
    }
};
