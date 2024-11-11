/**
 * Tests with concurrent DDL operation during query yield while runtime planning.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const db = conn.getDB(dbName);

assert(db.coll.drop());
assert(db.foreignColl.drop());

assert.commandWorked(db.coll.insert(Array.from({length: 10}, (_, i) => ({_id: i, a: i, b: i}))));
assert.commandWorked(
    db.foreignColl.insert(Array.from({length: 1}, (_, i) => ({_id: i, a: i, b: i}))));

assert.commandWorked(db.coll.createIndexes([{a: 1}, {b: 1}]));
assert.commandWorked(db.foreignColl.createIndex({x: 1}));

// Set it to a low value to easily yield during runtime planning
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));

/*
 * Drop a foreign collection index during query yield
 */
let fp = configureFailPoint(conn, "setYieldAllLocksHang", {namespace: db.coll.getFullName()});
let awaitShell =
    startParallelShell(funWithArgs(function(dbName, collName) {
                           const pipeline = [
                                {$match: {a: {$gte: 0}, b: {$gte: 0}}},
                                {$lookup: {from: "foreignColl", localField: "a", foreignField: "b", as: "out"}},
                                {$project: {a: 1, out: 1}}
                            ];
                           let result =
                               db.getSiblingDB(dbName)[collName].aggregate(pipeline).toArray();
                           assert(result.length === 10);
                       }, db.getName(), db.coll.getName()), conn.port);

fp.wait();
assert.commandWorked(db.foreignColl.dropIndex({x: 1}));
fp.off();
awaitShell();

/*
 * Rename the foreign collection during query yield
 */
fp = configureFailPoint(conn, "setYieldAllLocksHang", {namespace: db.coll.getFullName()});
awaitShell = startParallelShell(funWithArgs(function(dbName, collName) {
                                    const pipeline = [
                                        {$match: {a: {$gte: 0}, b: {$gte: 0}}},
                                        {$lookup: {from: "foreignColl", localField: "a", foreignField: "b", as: "out"}},
                                        {$project: {a: 1, out: 1}}
                                    ];
                                    assert.throwsWithCode(
                                        () => db.getSiblingDB(dbName)[collName].aggregate(pipeline),
                                        ErrorCodes.NamespaceNotFound);
                                }, db.getName(), db.coll.getName()), conn.port);

fp.wait();
assert.commandWorked(db.foreignColl.renameCollection("newColl"));
fp.off();
awaitShell();

/*
 * Drop and recreate the foreign collection during query yield
 */
// rename back
assert.commandWorked(db.newColl.renameCollection("foreignColl"));
fp = configureFailPoint(conn, "setYieldAllLocksHang", {namespace: db.coll.getFullName()});
awaitShell = startParallelShell(funWithArgs(function(dbName, collName) {
                                    const pipeline = [
                                        {$match: {a: {$gte: 0}, b: {$gte: 0}}},
                                        {$lookup: {from: "foreignColl", localField: "a", foreignField: "b", as: "out"}},
                                        {$project: {a: 1, out: 1}}
                                    ];
                                    assert.throwsWithCode(
                                        () => db.getSiblingDB(dbName)[collName].aggregate(pipeline),
                                        [ErrorCodes.NamespaceNotFound, ErrorCodes.QueryPlanKilled]);
                                }, db.getName(), db.coll.getName()), conn.port);

fp.wait();
assert(db.foreignColl.drop());
assert.commandWorked(db.foreignColl.insert({a: 0, b: 0}));
fp.off();
awaitShell();

MongoRunner.stopMongod(conn);
