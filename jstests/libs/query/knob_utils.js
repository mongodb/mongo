/*
 * Helpers for running a function with query knobs and/or failpoints temporarily set.
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {retryOnStorageChangeInterrupt} from "jstests/libs/run_with_retries.js";

function configureServerParameter(conn, name, value) {
    const {was} = assert.commandWorked(conn.adminCommand({setParameter: 1, [name]: value}));
    return () => configureServerParameter(conn, name, was);
}

/*
 * Runs the given function with the query knobs and/or failpoints set, then sets the values
 * back to their original state before exiting.
 * It's important that each run of the property is independent from one another, so we'll always
 * reset the knobs to their original state even if the function throws an exception.
 */
export function runWithKnobs(db, fn, knobToVal = {}, failPointToMode = {}) {
    const empty = (obj) => Object.keys(obj).length === 0;

    // If there are no knobs or failpoints to change, return the result of the function since
    // there's no other work to do.
    if (empty(knobToVal) && empty(failPointToMode)) {
        return fn();
    }

    let rollbacks = [];
    function setUp() {
        rollbacks = [];
        for (const host of DiscoverTopology.findNonConfigNodes(db.getMongo())) {
            // Establish the connection and mark it for cleanup.
            const conn = new Mongo(host);
            rollbacks.push(() => conn.close());

            // Configure the failpoints and knobs on the connection, and mark them for cleanup.
            for (const [name, mode] of Object.entries(failPointToMode)) {
                const fp = configureFailPoint(conn, name, {} /* data */, mode);
                rollbacks.push(() => fp.off());
            }

            // Configure the knobs and mark them for cleanup.
            for (const [name, value] of Object.entries(knobToVal)) {
                const rollback = configureServerParameter(conn, name, value);
                rollbacks.push(rollback);
            }
        }
    }

    // Perform the rollbacks in reverse order to ensure that we clean up in the opposite order of setup.
    function tearDown() {
        rollbacks.reverse().forEach((rollback) => rollback());
    }

    return retryOnStorageChangeInterrupt(() => {
        try {
            setUp();
            return fn();
        } finally {
            tearDown();
        }
    });
}
