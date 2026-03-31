/**
 * Tests that createIndexes enforces index spec validation, and correctly updates the catalog on
 * success while leaving it unchanged on failure.
 *
 * @tags: [
 *   assumes_superuser_permissions,
 * ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {IndexUtils} from "jstests/libs/index_utils.js";

const dbName = jsTestName();
const testDb = db.getSiblingDB(dbName);
const collName = "collTest";
const coll = testDb.getCollection(collName);

const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);

describe("createIndexes", function () {
    beforeEach(function () {
        testDb.dropDatabase();
        assert.commandWorked(testDb.createCollection(collName));
    });

    describe("malformed index specs", function () {
        afterEach(function () {
            // Ensure that no indexes were created
            IndexUtils.assertIndexes(coll, [{_id: 1}]);
        });

        it("fails with an empty list of index specs", function () {
            const res = coll.runCommand("createIndexes", {indexes: []});
            assert.commandFailedWithCode(res, ErrorCodes.BadValue);
        });

        it("fails when the 'key' field is missing from an index spec", function () {
            const res = coll.runCommand("createIndexes", {indexes: [{}]});
            assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
        });

        it("does not create any indexes when any spec in the list is malformed", function () {
            const res = coll.runCommand("createIndexes", {indexes: [{}, {key: {m: 1}, name: "asd"}]});
            assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
        });

        it("fails with an unsupported index type", function () {
            const res = coll.runCommand("createIndexes", {indexes: [{key: {x: "invalid_index_type"}, name: "x_1"}]});
            assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);
        });

        it("fails when the index name is empty", function () {
            const res = coll.runCommand("createIndexes", {indexes: [{key: {x: 1}, name: ""}]});
            assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);
        });

        it("fails with index version v0", function () {
            const res = coll.runCommand("createIndexes", {indexes: [{key: {d: 1}, name: "d_1", v: 0}]});
            assert.commandFailed(res, "v0 index creation should fail");
        });

        it("fails with an invalid top-level field", function () {
            const res = coll.runCommand("createIndexes", {indexes: [{key: {e: 1}, name: "e_1"}], invalidField: 1});
            assert.commandFailedWithCode(res, ErrorCodes.IDLUnknownField);
        });

        it("fails with an invalid field in an index spec (version V2)", function () {
            const res = coll.runCommand("createIndexes", {
                indexes: [{key: {e: 1}, name: "e_1", v: 2, invalidField: 1}],
            });
            assert.commandFailedWithCode(res, ErrorCodes.InvalidIndexSpecificationOption);
        });

        it("fails with an invalid field in an index spec (version V1)", function () {
            const res = coll.runCommand("createIndexes", {
                indexes: [{key: {e: 1}, name: "e_1", v: 1, invalidField: 1}],
            });
            assert.commandFailedWithCode(res, ErrorCodes.InvalidIndexSpecificationOption);
        });

        it("fails with an index named '*'", function () {
            const res = coll.runCommand("createIndexes", {indexes: [{key: {star: 1}, name: "*"}]});
            assert.commandFailedWithCode(res, ErrorCodes.BadValue);
        });

        it("fails when an index key value is an empty string", function () {
            const res = coll.runCommand("createIndexes", {indexes: [{key: {f: ""}, name: "f_1"}]});
            assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);
        });

        it("fails with duplicate index names in the same request", function () {
            const res = coll.runCommand("createIndexes", {
                indexes: [
                    {key: {g: 1}, name: "myidx"},
                    {key: {h: 1}, name: "myidx"},
                ],
            });
            assert.commandFailedWithCode(res, ErrorCodes.IndexKeySpecsConflict);
        });
    });

    it("successfully creates a sparse index and updates the catalog", function () {
        assert.commandWorked(coll.runCommand("createIndexes", {indexes: [{key: {c: 1}, sparse: true, name: "c_1"}]}));
        IndexUtils.assertIndexes(coll, [{_id: 1}, {c: 1}]);
        assert.eq(1, coll.getIndexes().filter((z) => z.sparse).length);
    });

    it("successfully creates a v1 index explicitly", function () {
        assert.commandWorked(
            coll.runCommand("createIndexes", {indexes: [{key: {d: 1}, name: "d_1", v: 1}]}),
            "v1 index creation should succeed",
        );
        IndexUtils.assertIndexes(coll, [{_id: 1}, {d: 1}]);
    });

    it("createIndexes on a view fails with CollectionUUIDMismatch when collectionUUID is provided", function () {
        assert.commandWorked(testDb.createView("toApple", "apple", []));
        const res = testDb.runCommand({
            createIndexes: "toApple",
            collectionUUID: UUID(),
            indexes: [{name: "_id_hashed", key: {_id: "hashed"}}],
        });
        assert.commandFailedWithCode(res, ErrorCodes.CollectionUUIDMismatch);

        testDb.getCollection("toApple").drop();
    });

    it("createIndexes on a view fails with CommandNotSupportedOnView when no collectionUUID is provided", function () {
        assert.commandWorked(testDb.createView("toApple", "apple", []));
        const res = testDb.runCommand({
            createIndexes: "toApple",
            indexes: [{name: "_id_hashed", key: {_id: "hashed"}}],
        });
        assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupportedOnView);
    });

    describe("User is not allowed to create indexes in config.transactions", function () {
        it("createIndexes on config.transactions fails with IllegalOperation", function () {
            const configDB = db.getSiblingDB("config");
            const res = configDB.runCommand({
                createIndexes: "transactions",
                indexes: [{key: {star: 1}, name: "star"}],
            });
            assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
        });

        it("createIndexes on config.transactions fails with IllegalOperation even with an empty index list", function () {
            const configDB = db.getSiblingDB("config");
            const res = configDB.runCommand({createIndexes: "transactions", indexes: []});
            assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
        });
    });

    describe("Bits parameter must be stored as an integer", function () {
        it("bits parameter is stored as an integer", function () {
            // TODO SERVER-120350: Remove this once v9.0 becomes last LTS
            if (isMultiversion) {
                jsTestLog(
                    "Skipping test when running on mixed binary versions because the bits parameter may have been stored as a non-int",
                );
                return;
            }

            assert.commandWorked(
                coll.runCommand("createIndexes", {indexes: [{key: {loc: "2d"}, name: "loc_2d", bits: 11.6}]}),
            );
            IndexUtils.assertIndexExists(coll, {loc: "2d"}, {bits: 11});
            assert(
                !IndexUtils.indexExists(coll, {loc: "2d"}, {bits: 11.6}),
                "index with non-int bits should not exist",
            );
        });
    });
});
