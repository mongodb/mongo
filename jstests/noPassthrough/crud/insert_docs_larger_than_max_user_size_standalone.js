// Test that by enabling the undocumented 'allowDocumentsGreaterThanMaxUserSize' server parameter,
// users can insert documents larger than the 16mb BSON user maximum size. Note that the node must
// be in standalone mode, and the document must not exceed the 16mb + 16kb internal maximum size.
// @tags: [requires_persistence, requires_replication]

import {ReplSetTest} from "jstests/libs/replsettest.js";

(function() {
"use strict";
const rst = new ReplSetTest({name: jsTestName(), nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("db");
const coll = db.getCollection("coll");

// Define the maximum size variables.
const bsonMaxUserSize = assert.commandWorked(db.hello()).maxBsonObjectSize;
assert.eq(bsonMaxUserSize, 16 * 1024 * 1024);
const bsonMaxInternalSize = bsonMaxUserSize + (16 * 1024);

// Trying to insert an object that is the maximum size will fail.
let obj = {x: 'x'.repeat(bsonMaxUserSize)};
assert.commandFailedWithCode(coll.insert(obj), ErrorCodes.BadValue, "object to insert too large");

// The string value in the field is a number of bytes smaller than the max, to account for other
// data in the BSON object. This value below will create an object very close to the maximum user
// size.
obj = {
    x: 'x'.repeat(16777186)
};
let size = Object.bsonsize(obj);
// The document to insert is a few bytes smaller than the maximum user size for a BSON object.
assert(size < bsonMaxUserSize);
assert.commandWorked(coll.insert(obj));

let oplog = primary.getDB("local").getCollection('oplog.rs');
let lastOplogEntry = oplog.find().sort({ts: -1}).limit(1).toArray()[0];

size = Object.bsonsize(lastOplogEntry);
// The size of the entire oplog entry is greater than the maximum user size for a BSON object,
// but it is still less than the internal maximum size.
assert(size > bsonMaxUserSize);
assert(size < bsonMaxInternalSize);

const dbpath = rst.getDbPath(primary);
rst.stop(0, undefined /* signal */, undefined /* opts */, {forRestart: true});

// Trying to start a replica set node with 'allowDocumentsGreaterThanMaxUserSize' will fail, as the
// parameter is only allowed in standalone mode.
let conn;
try {
    conn = MongoRunner.runMongod({
        dbpath: dbpath,
        replSet: jsTestName(),
        noCleanData: true,
        setParameter: {allowDocumentsGreaterThanMaxUserSize: true}
    });
} catch (err) {
    assert.eq(err.returnCode, MongoRunner.EXIT_BADOPTIONS);
}

// A standalone started without the parameter will still fail the insert of the large oplog entry.
conn = rst.start(0, {
    noReplSet: true,
    noCleanData: true,
});
oplog = conn.getDB("local").getCollection('oplog.rs');
assert.commandFailedWithCode(
    oplog.insert(lastOplogEntry), ErrorCodes.BadValue, "object to insert too large");
rst.stop(0, undefined /* signal */, undefined /* opts */, {forRestart: true});

// Restart as standalone with the 'allowDocumentsGreaterThanMaxUserSize' server parameter enabled to
// allow inserting documents larger than 16mb, as well as writes into the oplog.
conn = rst.start(0, {
    noReplSet: true,
    noCleanData: true,
    setParameter: {allowDocumentsGreaterThanMaxUserSize: true}
});

oplog = conn.getDB("local").getCollection('oplog.rs');
// With the parameter enabled, the insert should succeed.
assert.commandWorked(oplog.insert(lastOplogEntry));

// Reading back this value should also succeed.
lastOplogEntry = oplog.find().sort({ts: -1}).limit(1).toArray()[0];
size = Object.bsonsize(lastOplogEntry);
assert(size > bsonMaxUserSize);
assert(size < bsonMaxInternalSize);

rst.stopSet();
})();
