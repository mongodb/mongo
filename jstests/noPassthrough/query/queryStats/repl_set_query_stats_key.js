/**
 * This test confirms that queryStats store key fields specific to replica sets (readConcern and
 * readPreference) are included and correctly shapified. General command fields related to api
 * versioning are included for good measure.
 * @tags: [requires_fcv_71]
 */
import {getLatestQueryStatsEntry} from "jstests/libs/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({
    name: 'queryStatsTest',
    nodes: [
        {rsConfig: {tags: {dc: "east"}}},
        {rsConfig: {tags: {dc: "west"}}},
    ]
});

// Turn on the collecting of query stats metrics.
replTest.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
replTest.initiate();

const primary = replTest.getPrimary();

const dbName = jsTestName();
const collName = "foobar";
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

primaryColl.drop();

const clusterTime =
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{a: 1000}]}))
        .operationTime;

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
    $readPreference: {mode: "nearest", tags: [{some: "tag"}, {dc: "north pole"}, {dc: "east"}]},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
};
const replSetConn = new Mongo(replTest.getURL());
assert.commandWorked(replSetConn.getDB(dbName).runCommand(commandObj));
let stats = getLatestQueryStatsEntry(replSetConn, {collName: collName});
delete stats.key["collectionType"];
confirmCommandFieldsPresent(stats.key, commandObj);
// Check that readConcern afterClusterTime is normalized.
assert.eq(stats.key.readConcern.afterClusterTime, "?timestamp", tojson(stats.key.readConcern));

// Check that $readPreference.tags are sorted.
assert.eq(stats.key.$readPreference.tags,
          [{dc: "east"}, {dc: "north pole"}, {some: "tag"}],
          tojson(stats.key.$readPreference));

// Check that readConcern just has an afterClusterTime field.
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

// Check that readConcern has no afterClusterTime and fields related to api usage are not present.
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

// Check that the 'atClusterTime' parameter is shapified correctly.
commandObj.readConcern = {
    level: "snapshot",
    atClusterTime: clusterTime
},
    assert.commandWorked(replSetConn.getDB(dbName).runCommand(commandObj));
stats = getLatestQueryStatsEntry(replSetConn, {collName: collName});
assert.eq(stats.key["readConcern"], {level: "snapshot", atClusterTime: "?timestamp"});

replTest.stopSet();
