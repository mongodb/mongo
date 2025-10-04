/**
 * Test killing the secondary during initially sync
 *
 * 1. Bring up set
 * 2. Insert some data
 * 4. Make sure synced
 * 5. Freeze #2
 * 6. Bring up #3
 * 7. Kill #2 in the middle of syncing
 * 8. Eventually it should become a secondary
 * 9. Bring #2 back up
 * 10. Insert some stuff
 * 11. Everyone happy eventually
 *
 * This test assumes a 'newlyAdded' removal.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconnect} from "jstests/replsets/rslib.js";

let basename = "jstests_initsync1";

print("1. Bring up set");
// SERVER-7455, this test is called from ssl/auth_x509.js
let x509_options1;
let x509_options2;
let replTest = new ReplSetTest({name: basename, nodes: {node0: x509_options1, node1: x509_options2}});

let conns = replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
let foo = primary.getDB("foo");
let admin = primary.getDB("admin");

let secondary1 = replTest.getSecondary();
let admin_s1 = secondary1.getDB("admin");
let local_s1 = secondary1.getDB("local");

print("2. Insert some data");
let bulk = foo.bar.initializeUnorderedBulkOp();
for (var i = 0; i < 100; i++) {
    bulk.insert({date: new Date(), x: i, str: "all the talk on the market"});
}
assert.commandWorked(bulk.execute());
print("total in foo: " + foo.bar.find().itcount());

print("4. Make sure synced");
replTest.awaitReplication();

print("5. Freeze #2");
admin_s1.runCommand({replSetFreeze: 999999});

print("6. Bring up #3");
let hostname = getHostName();

let secondary2 = MongoRunner.runMongod(
    Object.merge(
        {
            replSet: basename,
            oplogSize: 2,
            // Preserve the initial sync state to validate an assertion.
            setParameter: {"failpoint.skipClearInitialSyncState": tojson({mode: "alwaysOn"})},
        },
        x509_options2,
    ),
);

let local_s2 = secondary2.getDB("local");
let admin_s2 = secondary2.getDB("admin");

let config = replTest.getReplSetConfig();
config.version = replTest.getReplSetConfigFromNode().version + 1;
config.members.push({_id: 2, host: secondary2.host});
try {
    admin.runCommand({replSetReconfig: config});
} catch (e) {
    print(e);
}
reconnect(secondary1);
reconnect(secondary2);
replTest.waitForAllNewlyAddedRemovals();

print("Config 1: " + tojsononeline(config));
let config2 = local_s1.system.replset.findOne();
print("Config 2: " + tojsononeline(config2));
assert(config2);
// Add one to config.version to account for the 'newlyAdded' removal.
assert.eq(config2.version, config.version + 1);

let config3 = local_s2.system.replset.findOne();
print("Config 3: " + tojsononeline(config3));
assert(config3);
assert.eq(config3.version, config.version + 1);

replTest.waitForState(secondary2, [ReplSetTest.State.SECONDARY, ReplSetTest.State.RECOVERING]);

print("7. Kill the secondary in the middle of syncing");
replTest.stop(secondary1);

print("8. Eventually the new node should become a secondary");
print("if initial sync has started, this will cause it to fail and sleep for 5 minutes");
replTest.awaitSecondaryNodes(60 * 1000, [secondary2]);

print("9. Bring the secondary back up");
replTest.start(secondary1, {}, true);
reconnect(secondary1);
replTest.waitForState(secondary1, [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY]);

print("10. Insert some stuff");
primary = replTest.getPrimary();
bulk = foo.bar.initializeUnorderedBulkOp();
for (var i = 0; i < 100; i++) {
    bulk.insert({date: new Date(), x: i, str: "all the talk on the market"});
}
assert.commandWorked(bulk.execute());

print("11. Everyone happy eventually");
replTest.awaitReplication();

MongoRunner.stopMongod(secondary2);
replTest.stopSet();
