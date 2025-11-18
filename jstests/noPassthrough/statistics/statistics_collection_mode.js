/**
 * Tests that the statistics collection mode can be configured.
 *
 * @tags: [
 *      requires_persistence,
 *      requires_wiredtiger
 * ]
 */

let mongo = MongoRunner.runMongod({});
const dbpath = mongo.dbpath;
let db = mongo.getDB("test");
let coll = db.getCollection("stats");

assert.commandWorked(coll.insert({x: 1}));
assert.commandWorked(db.adminCommand({fsync: 1}));

// Metric is zeroed out in the default collection mode (fast).
let stats = assert.commandWorked(coll.stats());
assert.eq(0, stats.wiredTiger.btree["row-store leaf pages"]);

MongoRunner.stopMongod(mongo);

mongo = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, wiredTigerStatisticsSetting: "all"});
db = mongo.getDB("test");
coll = db.getCollection("stats");

stats = assert.commandWorked(coll.stats());
assert.gt(stats.wiredTiger.btree["row-store leaf pages"], 0);

MongoRunner.stopMongod(mongo);
