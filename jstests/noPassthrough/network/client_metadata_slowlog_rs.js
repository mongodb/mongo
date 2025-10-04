/**
 * Test that verifies client metadata is logged as part of slow query logging in MongoD in a replica
 * set.
 * @tags: [
 *   requires_replication,
 *   requires_scripting,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const numNodes = 2;
const rst = new ReplSetTest({nodes: numNodes});

rst.startSet();
rst.initiate();
rst.awaitReplication();

// Build a new connection based on the replica set URL
let conn = new Mongo(rst.getURL(), undefined, {gRPC: false});

let coll = conn.getCollection("test.foo");
assert.commandWorked(coll.insert({_id: 1}, {writeConcern: {w: numNodes, wtimeout: 5000}}));

const predicate =
    /Slow query.*test.foo.*"appName":"MongoDB Shell".*"command":{"find":"foo","filter":{"\$where":{"\$code":"function\(\)/;

// Do a really slow query beyond the 100ms threshold
let count = coll.find({
                    $where: function() {
                        sleep(1000);
                        return true;
                    }
                })
                .readPref('primary')
                .toArray();
assert.eq(count.length, 1, "expected 1 document");
assert(checkLog.checkContainsOnce(rst.getPrimary(), predicate));

// Do a really slow query beyond the 100ms threshold
count = coll.find({
                $where: function() {
                    sleep(1000);
                    return true;
                }
            })
            .readPref('secondary')
            .toArray();
assert.eq(count.length, 1, "expected 1 document");
assert(checkLog.checkContainsOnce(rst.getSecondary(), predicate));

rst.stopSet();
