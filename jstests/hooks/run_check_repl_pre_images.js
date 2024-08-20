// Runner for checkPreImageCollection() that compares the pre-images collection on all replica set
// nodes to ensure all nodes have compatible data without any holes.

import {ReplSetTest} from "jstests/libs/replsettest.js";

const startTime = Date.now();
assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');

let runCheckOnReplSet = function(db) {
    // Always use admin database, this is important when running on multitenant suites, to bypass
    // the tenantId requirement.
    let adminDB = db.getSiblingDB('admin');
    let primaryInfo = adminDB.isMaster();

    assert(primaryInfo.ismaster,
           'shell is not connected to the primary or master node: ' + tojson(primaryInfo));

    let testFixture = new ReplSetTest(db.getMongo().host);
    testFixture.checkPreImageCollection("Pre-image consistency");
};

if (db.getMongo().isMongos()) {
    let configDB = db.getSiblingDB('config');

    // Run check on every shard.
    configDB.shards.find().forEach(shardEntry => {
        let newConn = new Mongo(shardEntry.host);
        runCheckOnReplSet(newConn.getDB('test'));
    });
} else {
    runCheckOnReplSet(db);
}

const totalTime = Date.now() - startTime;
print('Finished pre-image consistency checks of cluster in ' + totalTime + ' ms.');
