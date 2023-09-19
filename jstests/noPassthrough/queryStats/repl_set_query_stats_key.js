/**
 * This test confirms that queryStats store key fields specific to replica sets (readConcern and
 * readPreference) are included and correctly shapified. General command fields related to api
 * versioning are included for good measure.
 * @tags: [requires_fcv_71]
 */
import {getLatestQueryStatsEntry, getQueryStats} from "jstests/libs/query_stats_utils.js";

const replTest = new ReplSetTest({name: 'reindexTest', nodes: 2});

// Turn on the collecting of query stats metrics.
replTest.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
replTest.initiate();

const primary = replTest.getPrimary();

const dbName = jsTestName();
const collName = "foobar";
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

primaryColl.drop();

assert.commandWorked(primaryColl.insert({a: 1000}));

replTest.awaitReplication();

function confirmCommandFieldsPresent(queryStatsKey, commandObj) {
    for (const field in queryStatsKey) {
        if (field == "queryShape" || field == "client" || field == "command") {
            continue;
        }
        assert(commandObj.hasOwnProperty(field),
               `${field} is present in the query stats key but not present in command obj: ${
                   tojson(queryStatsKey)}, ${tojson(commandObj)}`);
    }
    assert.eq(Object.keys(queryStatsKey).length, Object.keys(commandObj).length, queryStatsKey);
}

let commandObj = {
    find: collName,
    filter: {v: {$eq: 2}},
    readConcern: {level: "local", afterClusterTime: new Timestamp(0, 1)},
    $readPreference: {mode: "primary"},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
};
const replSetConn = new Mongo(replTest.getURL());
assert.commandWorked(replSetConn.getDB(dbName).runCommand(commandObj));
let stats = getLatestQueryStatsEntry(replSetConn, {collName: collName});
delete stats.key["collectionType"];
confirmCommandFieldsPresent(stats.key, commandObj);
// check that readConcern afterClusterTime is normalized.
assert.eq(stats.key.readConcern.afterClusterTime, "?timestamp", stats.key.readConcern);

// check that readPreference not populated and readConcern just has an afterClusterTime field.
commandObj["readConcern"] = {
    afterClusterTime: new Timestamp(1, 0)
};
delete commandObj["$readPreference"];
assert.commandWorked(replSetConn.getDB(dbName).runCommand(commandObj));
stats = getLatestQueryStatsEntry(replSetConn, {collName});
// We're not concerned with this field here.
delete stats.key["collectionType"];
confirmCommandFieldsPresent(stats.key, commandObj);
assert.eq(stats.key["readConcern"], {"afterClusterTime": "?timestamp"});

// check that readConcern has no afterClusterTime and fields related to api usage are not present.
commandObj["readConcern"] = {
    level: "local"
};
delete commandObj["apiDeprecationErrors"];
delete commandObj["apiVersion"];
delete commandObj["apiStrict"];
assert.commandWorked(replSetConn.getDB(dbName).runCommand(commandObj));
stats = getLatestQueryStatsEntry(replSetConn, {collName: collName});
assert.eq(stats.key["readConcern"], {level: "local"});
// We're not concerned with this field here.
delete stats.key["collectionType"];
confirmCommandFieldsPresent(stats.key, commandObj);

replTest.stopSet();
