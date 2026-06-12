/**
 * Tests that collMod blocks upgrading validationLevel to 'constraint' when the sharded
 * collection contains documents that violate the validator, and allows the upgrade when all
 * documents conform. The scan is performed by the coordinator before pausing migrations.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("collMod upgrade to constraint validationLevel on sharded collections", function () {
    let st;
    let testDb;
    const dbName = jsTestName();
    const collName = "test";
    const validator = {a: {$exists: true}};

    before(function () {
        st = new ShardingTest({shards: 2});
        testDb = st.s.getDB(dbName);

        // Pin the coordinator to shard0 by making it the primary shard.
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(
            testDb.createCollection(collName, {
                validator: validator,
                validationLevel: "strict",
                validationAction: "warn",
            }),
        );

        // Split: _id < 0 on shard0 (coordinator), _id >= 0 on shard1.
        st.shardColl(collName, {_id: 1}, {_id: 0}, {_id: 1}, dbName, true);

        assert.commandWorked(testDb[collName].insert({_id: -1, a: 1}));
        assert.commandWorked(testDb[collName].insert({_id: 1, a: 1}));
    });

    after(function () {
        st.stop();
    });

    it("blocks upgrade when a document on the non-coordinating shard violates the validator", function () {
        // _id: 2 >= 0 routes to shard1 (non-coordinator). Succeeds because action is "warn".
        assert.commandWorked(testDb[collName].insert({_id: 2, b: 1}));

        const res = assert.commandFailedWithCode(
            testDb.runCommand({
                collMod: collName,
                validationLevel: "constraint",
                validationAction: "error",
            }),
            12370902,
        );
        assert(
            res.errmsg.includes("Cannot upgrade validationLevel to 'constraint'"),
            "expected scan-based error",
            {
                res,
            },
        );
        assert(res.errmsg.includes("db." + collName + ".find("), "expected find-query suggestion", {
            res,
        });

        assert.commandWorked(testDb[collName].deleteOne({_id: 2}));
    });

    it("allows upgrade when all documents conform", function () {
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validationLevel: "strict",
                validationAction: "warn",
            }),
        );
        assert.commandWorked(
            testDb.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validationLevel: "constraint",
                validationAction: "error",
            }),
        );

        // After upgrade, compliant inserts still work.
        assert.commandWorked(testDb[collName].insert({_id: 3, a: 1}));

        // After upgrade, non-compliant inserts are rejected.
        assert.commandFailedWithCode(
            testDb[collName].insert({_id: 4, b: 1}),
            ErrorCodes.DocumentValidationFailure,
        );
    });

    it("blocks upgrade when a document on the coordinator shard violates the validator", function () {
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validationLevel: "strict",
                validationAction: "warn",
            }),
        );

        // _id: -2 < 0 routes to shard0, which is the coordinator (primary) shard.
        assert.commandWorked(testDb[collName].insert({_id: -2, b: 1}));

        assert.commandFailedWithCode(
            testDb.runCommand({
                collMod: collName,
                validationLevel: "constraint",
                validationAction: "error",
            }),
            12370902,
        );

        assert.commandWorked(testDb[collName].deleteOne({_id: -2}));
        assert.commandWorked(
            testDb.runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
        );
        assert.commandWorked(
            testDb.runCommand({
                collMod: collName,
                validationLevel: "constraint",
                validationAction: "error",
            }),
        );
    });
});
