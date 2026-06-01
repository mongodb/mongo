/*
 * Verifies the $listClusterCatalog will respect the set maxTimeMS while running internally
 * listCollection.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDbs = ["db1", "db2"];

function runTest(conn, failPointNode, timeoutMs) {
    // Create at least 2 dbs to make sure the $_internalListCollection will run 2 iterations of
    // listCollection.
    kDbs.forEach((dbName) => {
        conn.getDB(dbName).createCollection("coll1");
    });

    let fp = configureFailPoint(failPointNode, "hangBeforeListCollections");
    const awaitListClusterCatalog = startParallelShell(
        funWithArgs(function (timeout) {
            assert.commandFailedWithCode(
                db.adminCommand("aggregate", {pipeline: [{$listClusterCatalog: {}}], cursor: {}, maxTimeMS: timeout}),
                [ErrorCodes.MaxTimeMSExpired],
            );
        }, timeoutMs),
        conn.port,
    );

    // Run the first listCollection and wait for it to complete
    jsTest.log("Waiting for hangBeforeListCollections");
    fp.wait();

    // sleep timeoutMs so that maxTimeMs expires
    jsTest.log("Waiting for timeout", {timeoutMs});
    sleep(timeoutMs + 1);
    fp.off();

    // Run the second listCollection - The aggregation should throw.
    jsTest.log("Waiting for $listClusterCatalog to complete");
    awaitListClusterCatalog();
}

jsTest.log("Verify the maxTimeMs is respected for $listClusterCatalog on a replica set");
{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    runTest(rst.getPrimary(), rst.getPrimary(), 1000);
    rst.stopSet();
}

jsTest.log("Verify the maxTimeMs is respected for $listClusterCatalog in a cluster");
{
    const st = new ShardingTest({shards: 1});
    // The cluster path traverses mongos -> shard -> DBPrimaryRouter -> remote listCollections,
    // and can incur catalog/placement cache refreshes that each consume hundreds of ms on slow
    // debug builds. Use a more generous timeout so the aggregate reliably reaches the failpoint
    // before maxTimeMS expires from routing overhead alone; the bulk of the test wall time is
    // the deliberate sleep below, not the timeout itself.
    runTest(st.s, st.rs0.getPrimary(), 10000);
    st.stop();
}
