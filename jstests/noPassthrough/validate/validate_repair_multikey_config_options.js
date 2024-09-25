/**
 * Tests validate's 'repair' and 'fixMultikey' configuration options.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const compatibleOptions = [{repair: false, fixMultikey: true}, {repair: true, fixMultikey: true}];

const incompatibleOptions =
    [{repair: true, fixMultikey: false}, {fixMultikey: true, background: true}];

const incompatibleReplOptions = [{repair: true}, {fixMultikey: true, background: true}];

const runTest = (coll, options, expectFailure) => {
    const res = coll.validate(options);
    if (expectFailure) {
        assert.commandFailedWithCode(res,
                                     [ErrorCodes.InvalidOptions, ErrorCodes.CommandNotSupported]);
        return;
    }

    assert.commandWorked(res);
    assert(res.valid);
};

// Test standalone.
(() => {
    const conn = MongoRunner.runMongod();
    const dbName = jsTestName();
    const collName = 'test';
    const db = conn.getDB(dbName);

    assert.commandWorked(db.createCollection(collName));
    const coll = db.getCollection(collName);

    for (const options of compatibleOptions) {
        runTest(coll, options, /* expectFailure=*/ false);
    }

    for (const options of incompatibleOptions) {
        runTest(coll, options, /* expectFailure=*/ true);
    }

    MongoRunner.stopMongod(conn);
})();

// Test replica set.
(() => {
    const replSet = new ReplSetTest({nodes: 1});
    replSet.startSet();
    replSet.initiate();
    const dbName = jsTestName();
    const collName = 'test';
    const primary = replSet.getPrimary();
    const db = primary.getDB(dbName);

    assert.commandWorked(db.createCollection(collName));
    const coll = db.getCollection(collName);

    for (const options of incompatibleReplOptions) {
        runTest(coll, options, /* expectFailure=*/ true);
    }

    replSet.stopSet();
})();
