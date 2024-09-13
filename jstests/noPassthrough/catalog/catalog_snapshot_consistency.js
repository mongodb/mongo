/**
 * Test local catalog for commands like listDatabases, focusing on consistency with the durable
 * storage snapshot.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
});
replTest.startSet();
replTest.initiate();
let mongod = replTest.getPrimary();

const slowPublishDb = "catalog_snapshot_consistency_slow_publish_db";
const slowPublishColl = "coll";

// List database should reflect an implicitly created database that has been committed but not
// published into the local catalog yet. Use a failpoint to hang before publishing the catalog,
// simulating a slow catalog publish.
const failPoint = configureFailPoint(mongod,
                                     "hangBeforePublishingCatalogUpdates",
                                     {collectionNS: slowPublishDb + '.' + slowPublishColl});
const waitDbCreate = startParallelShell(`{
    db.getSiblingDB('${slowPublishDb}')['${slowPublishColl}'].createIndex({a:1});
}`, mongod.port);
failPoint.wait();

let cmdRes = assert.commandWorked(
    mongod.adminCommand({listDatabases: 1, filter: {$expr: {$eq: ["$name", slowPublishDb]}}}));
assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
assert.eq(slowPublishDb, cmdRes.databases[0].name, tojson(cmdRes));

failPoint.off();
waitDbCreate();

replTest.stopSet();
