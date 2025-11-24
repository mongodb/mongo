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
    });

    const tests = [
        {indexType: "2d"},
        {indexType: "2dsphere"},
        {indexType: "text"},
        {indexType: "hashed"},
        {indexType: 1, expectedType: "btree"},
        {indexType: 1, indexName: "$**", expectedType: "wildcard"},
        {
            indexType: "2dsphere",
            collectionOptions: {timeseries: {timeField: "t", metaField: "m"}},
            expectedType: "2dsphere_bucket",
        },
    ];

    tests.forEach(function (params) {
        const expectedType = params.expectedType || params.indexType;
        it(`contains indexType ${expectedType}`, function () {
            this.db.createCollection("coll", params.collectionOptions);
            this.db.coll.createIndex({[params.indexName || "foo"]: params.indexType});
            checkIndexTypeLogExists(this.db.coll, expectedType);
        });
    });

    afterEach(function () {
        this.db.coll.dropIndexes();
        this.db.coll.drop();
        clearRawMongoProgramOutput();
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });
});
