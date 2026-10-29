/**
 * SERVER-134863: a validator may be well formed on the binary version that wrote it but not on
 * the applying one. Oplog application tolerates such entries, the same way startup does; the
 * collection keeps the malformed validator so writes to it are rejected (fail closed) rather
 * than allowed unvalidated.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Invalid because '*' is a quantifier that does not follow a repeatable item.
const malformedValidator = {email: {$regex: "^*"}};

describe("malformed collection validator oplog application", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: 2});
        this.rst.startSet();
        this.rst.initiate();

        this.db = this.rst.getPrimary().getDB(jsTestName());
        this.failPoint = configureFailPoint(this.db, "allowSettingMalformedCollectionValidators");
    });

    after(function () {
        try {
            this.failPoint.off();
        } finally {
            this.rst.stopSet();
        }
    });

    it("tolerates a collMod oplog entry with a malformed validator", function () {
        assert.commandWorked(this.db.createCollection("collmodColl"));
        assert.commandWorked(
            this.db.runCommand({collMod: "collmodColl", validator: malformedValidator}),
        );
        this.rst.awaitReplication();
    });

    it("tolerates a create oplog entry with a malformed validator", function () {
        assert.commandWorked(
            this.db.createCollection("createColl", {validator: malformedValidator}),
        );
        this.rst.awaitReplication();
    });

    it("tolerates a collMod that re-parses a malformed validator", function () {
        assert.commandWorked(
            this.db.createCollection("reparseColl", {validator: malformedValidator}),
        );

        // Changing only the validation level re-parses the stored validator.
        assert.commandWorked(
            this.db.runCommand({collMod: "reparseColl", validationLevel: "moderate"}),
        );
        this.rst.awaitReplication();
    });

    it("tolerates a create with a malformed validator inside a transaction", function () {
        const session = this.db.getMongo().startSession();
        const sessionDb = session.getDatabase(this.db.getName());

        try {
            session.startTransaction();
            assert.commandWorked(
                sessionDb.createCollection("txnColl", {validator: malformedValidator}),
            );
            session.commitTransaction();
        } finally {
            session.endSession();
        }

        this.rst.awaitReplication();
    });

    it("tolerates malformed validator entries during initial sync", function () {
        assert.commandWorked(
            this.db.createCollection("initialSyncColl", {validator: malformedValidator}),
        );
        this.rst.awaitReplication();

        const initialSyncNode = this.rst.add({});
        this.rst.reInitiate();
        this.rst.awaitSecondaryNodes();
        this.rst.awaitReplication();

        assert.docEq(
            initialSyncNode.getDB(this.db.getName()).initialSyncColl.getMetadata().options
                .validator,
            malformedValidator,
        );
    });
});
