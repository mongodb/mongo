/**
 * Test that $queryStats properly tokenizes aggregation commands, on mongod and mongos.
 * @tags: [featureFlagQueryStats]
 */
load("jstests/libs/telemetry_utils.js");
(function() {
"use strict";

const kHashedDbName = "iDlS7h5jf5HHxWPJpeHRbA+jLTNNZaqxVVkplrEkfko=";
const kHashedCollName = "w6Ax20mVkbJu4wQWAMjL8Sl+DfXAr2Zqdc3kJRB7Oo0=";
const kHashedFieldA = "GDiF6ZEXkeo4kbKyKEAAViZ+2RHIVxBQV9S6b6Lu7gU=";
const kHashedFieldB = "m1xtUkfSpZNxXjNZYKwo86vGD37Zxmd2gtt+TXDO558=";

function runTest(conn) {
    const db = conn.getDB("testDB");
    const admin = conn.getDB("admin");

    db.test.drop();
    assert.commandWorked(db.test.insert({a: "foobar", b: 15}));
    assert.commandWorked(db.test.insert({a: "foobar", b: 20}));

    db.test
        .aggregate([
            {$sort: {a: -1}},
            {$match: {a: {$regex: "foo(.*)"}, b: {$gt: 10}}},
            {$skip: 5},
        ])
        .toArray();

    let stats = getTelemetry(admin);
    stats = getQueryStatsAggCmd(admin, /*applyHmacToIdentifiers*/ true);

    assert.eq(1, stats.length);
    assert.eq({"db": `${kHashedDbName}`, "coll": `${kHashedCollName}`},
              stats[0].key.queryShape.cmdNs);
    assert.eq("aggregate", stats[0].key.queryShape.command);
    assert.eq(
        [
            {"$sort": {[kHashedFieldA]: -1}},
            {
                "$match": {
                    "$and": [
                        {[kHashedFieldA]: {"$regex": "?string"}},
                        {[kHashedFieldB]: {"$gt": "?number"}}
                    ]
                }
            },
            {"$skip": "?"}
        ],
        stats[0].key.queryShape.pipeline);

    db.test.aggregate([{$match: {a: {$regex: "foo(.*)"}, b: {$gt: 0}}}]).toArray();
    stats = getQueryStatsAggCmd(admin, /*applyHmacToIdentifiers*/ true);

    assert.eq(2, stats.length);
    assert.eq({"db": `${kHashedDbName}`, "coll": `${kHashedCollName}`},
              stats[0].key.queryShape.cmdNs);
    assert.eq("aggregate", stats[0].key.queryShape.command);
    assert.eq([{
                  "$match": {
                      "$and": [
                          {[kHashedFieldA]: {"$regex": "?string"}},
                          {[kHashedFieldB]: {"$gt": "?number"}}
                      ]
                  }
              }],
              stats[0].key.queryShape.pipeline);
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsSamplingRate: -1,
        featureFlagQueryStats: true,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

// TODO SERVER-77325 reenable these tests
// const st = new ShardingTest({
//     mongos: 1,
//     shards: 1,
//     config: 1,
//     rs: {nodes: 1},
//     mongosOptions: {
//         setParameter: {
//             internalQueryStatsSamplingRate: -1,
//             'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
//         }
//     },
// });
// runTest(st.s);
// st.stop();
}());
