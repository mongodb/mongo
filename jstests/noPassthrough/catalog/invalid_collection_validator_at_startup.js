/**
 * Test that a node can start up despite the presence of an invalid collection validator that's on
 * disk (because it was written using a prior version of the server).
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */
import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = "invalid_collection_validator_at_startup";
const dbpath = MongoRunner.dataPath + testName;
const collName = "collectionWithMalformedValidator";

// Invalid because '*' is a quantifier that does not follow a repeatable item.
const malformedValidator = {email: {$regex: "^*"}};
const kValidatorParseErrorCode = 51091;
const document = {email: "x@example.com"};

describe("startup with a malformed collection validator on a standalone", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({dbpath});
        assert.neq(null, this.conn, "mongod was unable to start up");

        const testDB = this.conn.getDB(jsTestName());
        assert.commandWorked(testDB[collName].insert({a: "hello world"}));

        // Use collMod to give the collection a malformed validator.
        withFailPoint(this.conn, "allowSettingMalformedCollectionValidators", () => {
            assert.commandWorked(
                testDB.runCommand({collMod: collName, validator: malformedValidator}),
            );
        });
    });

    after(function () {
        if (this.conn) {
            MongoRunner.stopMongod(this.conn);
        }
    });

    it("retains the persisted validator and rejects writes after restart", function () {
        MongoRunner.stopMongod(this.conn);
        this.conn = MongoRunner.runMongod({dbpath, noCleanData: true});
        assert.neq(null, this.conn, "mongod was unable to start up");

        const testDB = this.conn.getDB(jsTestName());
        const cmdRes = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
        assert(/has malformed validator/.test(cmdRes.log));

        assert.docEq(testDB[collName].getMetadata().options.validator, malformedValidator);
        assert.commandFailedWithCode(testDB[collName].insert(document), kValidatorParseErrorCode);

        assert.commandWorked(testDB.someOtherCollection.insert({a: 1}));
    });
});

describe("startup with a malformed collection validator in a replica set", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet();
        this.rst.initiate();

        const primary = this.rst.getPrimary();
        const testDB = primary.getDB(jsTestName());

        // Retain the parse error in memory, as a secondary applying the oplog would.
        withFailPoint(
            primary,
            "allowSettingMalformedCollectionValidators",
            () => {
                assert.commandWorked(
                    testDB.createCollection(collName, {validator: malformedValidator}),
                );
            },
            {data: {parseValidator: true}},
        );
    });

    after(function () {
        this.rst.stopSet();
    });

    it("retains the persisted validator and rejects writes after restart", function () {
        // The failpoint is off, so startup must parse the validator stored on disk.
        this.rst.restart(this.rst.getPrimary());

        const testDB = this.rst.getPrimary().getDB(jsTestName());
        const cmdRes = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
        assert(/has malformed validator/.test(cmdRes.log));

        assert.docEq(testDB[collName].getMetadata().options.validator, malformedValidator);
        assert.commandFailedWithCode(testDB[collName].insert(document), kValidatorParseErrorCode);

        assert.commandWorked(testDB.someOtherCollection.insert({a: 1}));
    });
});
