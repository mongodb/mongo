import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

// Checks that the provided collection logs the provided index type during validation.
function checkIndexTypeLogExists(coll, indexType) {
    coll.validate();
    assert.soon(function () {
        const logs = rawMongoProgramOutput('"id":20296').split("\n");
        return logs.some((line) => line.includes(`"indexType":"${indexType}"`));
    });
}

describe("Index validation log message", function () {
    before(function () {
        this.conn = MongoRunner.runMongod();
        this.db = this.conn.getDB("test");

        // Set verbosity to 1 to show otherwise production-only logs.
        assert.commandWorked(this.db.adminCommand({setParameter: 1, logComponentVerbosity: {index: {verbosity: 1}}}));

        this.db.createCollection("coll");
    });

    it("contains indexType 2d", function () {
        this.db.coll.createIndex({"foo": "2d"});
        checkIndexTypeLogExists(this.db.coll, "2d");
    });

    it("contains indexType 2dsphere", function () {
        this.db.coll.createIndex({"foo": "2dsphere"});
        checkIndexTypeLogExists(this.db.coll, "2dsphere");
    });

    it("contains indexType 2dsphere_bucket", function () {
        this.db.coll.createIndex({"foo": "2dsphere_bucket"});
        checkIndexTypeLogExists(this.db.coll, "2dsphere_bucket");
    });

    it("contains indexType text", function () {
        this.db.coll.createIndex({"foo": "text"});
        checkIndexTypeLogExists(this.db.coll, "text");
    });

    it("contains indexType hashed", function () {
        this.db.coll.createIndex({"foo": "hashed"});
        checkIndexTypeLogExists(this.db.coll, "hashed");
    });

    it("contains indexType btree", function () {
        this.db.coll.createIndex({"foo": 1});
        checkIndexTypeLogExists(this.db.coll, "btree");
    });

    it("contains indexType wildcard", function () {
        this.db.coll.createIndex({"$**": 1});
        checkIndexTypeLogExists(this.db.coll, "wildcard");
    });

    afterEach(function () {
        this.db.coll.dropIndexes();
        clearRawMongoProgramOutput();
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });
});
