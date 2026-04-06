/**
 * Tests that "collectionType" appears in slow query logs for write commands.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {checkCollectionType} from "jstests/noPassthrough/logging/log_collectionType_utils.js";

const dbName = jsTestName();
const collName = "testColl";

// TODO SERVER-122926 Add more coverage for different collection types, topologies, and command types, if applicable.
describe("collectionType in write command slow query logs", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({});
        this.db = this.conn.getDB(dbName);
        this.db.setProfilingLevel(0, -1);
        this.db[collName].drop();
        assert.commandWorked(this.db[collName].insertMany([{a: 1}, {a: 2}, {a: 3}]));
    });
    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("insert logs collectionType", function () {
        const comment = jsTestName() + "_insert";
        assert.commandWorked(this.db.runCommand({insert: collName, documents: [{b: 1}], comment}));
        checkCollectionType({db: this.db, comment, command: "insert", expectedCollType: "normal"});
    });

    it("update logs collectionType", function () {
        const comment = jsTestName() + "_update";
        assert.commandWorked(
            this.db.runCommand({update: collName, updates: [{q: {a: 1}, u: {$set: {b: 2}}}], comment}),
        );
        checkCollectionType({db: this.db, comment, command: "update", expectedCollType: "normal"});
    });

    it("delete logs collectionType", function () {
        const comment = jsTestName() + "_delete";
        assert.commandWorked(this.db.runCommand({delete: collName, deletes: [{q: {a: 2}, limit: 1}], comment}));
        checkCollectionType({db: this.db, comment, command: "delete", expectedCollType: "normal"});
    });

    it("findAndModify logs collectionType", function () {
        const comment = jsTestName() + "_findAndModify";
        assert.commandWorked(
            this.db.runCommand({findandmodify: collName, query: {a: 3}, update: {$set: {b: 4}}, comment}),
        );
        checkCollectionType({db: this.db, comment, command: "findAndModify", expectedCollType: "normal"});
    });
});
