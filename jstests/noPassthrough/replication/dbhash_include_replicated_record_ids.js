/**
 * Tests which collections dbHash's 'includeReplicatedRecordIds' opt-in applies to. RecordIds are
 * hashed only for collections that are in scope for replicated RecordIds and have the
 * 'recordIdsReplicated' option. All other collections hash the same with and without the opt-in.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("dbHash includeReplicatedRecordIds", function () {
    const dbName = jsTestName();

    before(function () {
        // Some cases inject RecordId divergence, which the end-of-test check would report.
        TestData.skipCheckDBHashes = true;
        this.rst = new ReplSetTest({nodes: 2});
        this.rst.startSet();
        this.rst.initiate();
        this.primary = this.rst.getPrimary();
        this.secondary = this.rst.getSecondary();
        this.primaryDB = this.primary.getDB(dbName);
        this.secondaryDB = this.secondary.getDB(dbName);
    });

    after(function () {
        this.rst.stopSet();
        delete TestData.skipCheckDBHashes;
    });

    function dbHash(db, collName, includeReplicatedRecordIds) {
        const cmd = {dbHash: 1, collections: [collName]};
        if (includeReplicatedRecordIds) {
            cmd.includeReplicatedRecordIds = 1;
        }
        return assert.commandWorked(db.runCommand(cmd));
    }

    function collHash(db, collName, includeReplicatedRecordIds) {
        const res = dbHash(db, collName, includeReplicatedRecordIds);
        assert(res.collections.hasOwnProperty(collName), "collection missing from dbHash", {res});
        return res.collections[collName];
    }

    // Inserts 'doc' on the primary, and makes the secondary store it under a different RecordId.
    function insertWithDivergentRecordId(rst, primaryDB, secondaryDB, collName, doc) {
        const fp = configureFailPoint(secondaryDB, "explicitlySetRecordIdOnInsert", {
            doc,
            rid: 400,
        });
        assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [doc]}));
        rst.awaitReplication();
        fp.off();
    }

    function createWithDivergentRecordId(ctx, collName, {recordIdsReplicated}) {
        const fp = recordIdsReplicated
            ? null
            : configureFailPoint(ctx.primary, "overrideRecordIdsReplicatedFalse");
        assert.commandWorked(ctx.primaryDB.createCollection(collName));
        fp?.off();
        assert.commandWorked(ctx.primaryDB[collName].insert({_id: 1, a: 1}));
        ctx.rst.awaitReplication();
        insertWithDivergentRecordId(ctx.rst, ctx.primaryDB, ctx.secondaryDB, collName, {
            _id: 2,
            a: 2,
        });
    }

    it("hashes RecordIds of collections with the option", function () {
        const collName = "withOption";
        createWithDivergentRecordId(this, collName, {recordIdsReplicated: true});

        assert.neq(
            collHash(this.primaryDB, collName, true),
            collHash(this.primaryDB, collName, false),
        );
        assert.neq(
            collHash(this.primaryDB, collName, true),
            collHash(this.secondaryDB, collName, true),
        );
        assert.eq(
            collHash(this.primaryDB, collName, false),
            collHash(this.secondaryDB, collName, false),
        );
    });

    it("does not hash RecordIds of collections without the option", function () {
        const collName = "withoutOption";
        createWithDivergentRecordId(this, collName, {recordIdsReplicated: false});

        assert.eq(
            collHash(this.primaryDB, collName, true),
            collHash(this.primaryDB, collName, false),
        );
        assert.eq(
            collHash(this.primaryDB, collName, true),
            collHash(this.secondaryDB, collName, true),
        );
    });

    it("does not hash RecordIds of clustered collections", function () {
        const collName = "clustered";
        assert.commandWorked(
            this.primaryDB.createCollection(collName, {
                clusteredIndex: {key: {_id: 1}, unique: true},
            }),
        );
        assert.commandWorked(this.primaryDB[collName].insert([{_id: 1}, {_id: 2}]));
        this.rst.awaitReplication();

        assert.eq(
            collHash(this.primaryDB, collName, true),
            collHash(this.primaryDB, collName, false),
        );
        assert.eq(
            collHash(this.primaryDB, collName, true),
            collHash(this.secondaryDB, collName, true),
        );
    });

    it("does not hash RecordIds of implicitly replicated namespaces", function () {
        const collName = "retryable";
        const session = this.primary.startSession({retryWrites: true});
        const sessionColl = session.getDatabase(dbName)[collName];
        assert.commandWorked(sessionColl.insert({_id: 1, a: 1}));
        // A retryable findAndModify writes its pre-image to config.image_collection.
        sessionColl.findAndModify({query: {_id: 1}, update: {$inc: {a: 1}}});
        session.endSession();
        this.rst.awaitReplication();

        const configDB = this.primary.getDB("config");
        for (const configCollName of ["transactions", "image_collection"]) {
            assert.eq(
                collHash(configDB, configCollName, true),
                collHash(configDB, configCollName, false),
                "opt-in changed the hash",
                {configCollName},
            );
        }
    });

    it("keeps the response format", function () {
        const collName = "withOption";
        const fields = (res) => Object.keys(res).sort();
        assert.eq(
            fields(dbHash(this.primaryDB, collName, true)),
            fields(dbHash(this.primaryDB, collName, false)),
        );
    });
});
