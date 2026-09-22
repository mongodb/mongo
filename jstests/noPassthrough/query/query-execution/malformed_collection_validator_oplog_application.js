/**
 * SERVER-134863: a validator may be well formed on the binary version that wrote it but not on
 * the applying one. Oplog application tolerates such entries, the same way startup does; the
 * collection keeps the malformed validator so writes to it are rejected (fail closed) rather
 * than allowed unvalidated. The failpoint simulates a primary running such a version.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Invalid because '*' is a quantifier that does not follow a repeatable item.
const malformedValidator = {email: {$regex: "^*"}};
const kValidatorParseErrorCode = 51091;

describe("malformed collection validator oplog application", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: 2});
        this.rst.startSet();
        this.rst.initiate();
        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB("test");
        this.failPoint = configureFailPoint(
            this.primary,
            "allowSettingMalformedCollectionValidators",
        );
    });

    after(function () {
        this.rst.stopSet();
    });

    function assertOplogAppliedAndNodesAlive(rst) {
        rst.awaitReplication();
        rst.nodes.forEach((node) =>
            assert.commandWorked(node.getDB("admin").runCommand({ping: 1})),
        );
    }

    it("tolerates a collMod oplog entry with a malformed validator", function () {
        assert.commandWorked(this.db.createCollection("collmodColl"));
        assert.commandWorked(
            this.db.runCommand({collMod: "collmodColl", validator: malformedValidator}),
        );
        assertOplogAppliedAndNodesAlive(this.rst);
    });

    it("tolerates a create oplog entry with a malformed validator", function () {
        assert.commandWorked(
            this.db.createCollection("createColl", {validator: malformedValidator}),
        );
        assertOplogAppliedAndNodesAlive(this.rst);
    });

    it("tolerates a collMod that re-parses a malformed validator", function () {
        // Changing only the validation level re-parses the stored validator.
        assert.commandWorked(
            this.db.runCommand({collMod: "collmodColl", validationLevel: "moderate"}),
        );
        assertOplogAppliedAndNodesAlive(this.rst);
    });

    it("tolerates a create with a malformed validator inside a transaction", function () {
        const session = this.primary.startSession();
        const sessionDb = session.getDatabase("test");
        session.startTransaction();
        assert.commandWorked(
            sessionDb.createCollection("txnColl", {validator: malformedValidator}),
        );
        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();
        assertOplogAppliedAndNodesAlive(this.rst);
    });

    it("tolerates malformed validator entries during initial sync", function () {
        this.rst.add({});
        this.rst.reInitiate();
        this.rst.awaitSecondaryNodes();
        assertOplogAppliedAndNodesAlive(this.rst);
    });

    it("keeps the broken validator and fails subsequent writes", function () {
        this.failPoint.off();

        // Step up a node that failed to parse the validators for real (no failpoint there).
        this.rst.stepUp(this.rst.getSecondary());
        const db = this.rst.getPrimary().getDB("test");

        for (const collName of ["collmodColl", "createColl", "txnColl"]) {
            assert.commandFailedWithCode(
                db[collName].insert({email: "x"}),
                kValidatorParseErrorCode,
            );
            assert.docEq(
                db.getCollectionInfos({name: collName})[0].options.validator,
                malformedValidator,
                collName,
            );
        }

        assert.commandWorked(db.unrelated.insert({a: 1}));
    });
});
