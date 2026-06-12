/**
 * FSM stress test for concurrent createIndexes and FCV downgrade, with regression coverage
 * for SERVER-125400.
 *
 * Multiple threads concurrently create and drop indexes of various types while other threads
 * cycle the FCV between latestFCV and lastLTSFCV. Covered types: 2dsphere, 2d, text,
 * wildcard, hashed, plain single-field, compound (2–4 fields, mixed sort directions),
 * geo+regular compounds, compound-with-text, compound-hashed, and partial-filter.
 *
 * The primary regression target is the 2dsphere v4 case (SERVER-125400): without the
 * FixedOperationFCVRegion fix in createIndexes, a secondary could fassert (msgid 34437)
 * when replaying a v4 createIndexes oplog entry at transitional FCV, because
 * featureFlag2dsphereIndexVersion4 has enableOnTransitionalFCV=false. On FCV versions
 * where the flag is disabled, the workload degrades gracefully (2dsphere creates v3
 * indexes) while still stressing the other index types.
 *
 * Even-tid threads pre-insert a document so their builds take the two-phase async path,
 * covering ForwardableOperationMetadata OFCV propagation. Odd-tid threads leave their
 * collections empty (single-phase path). If the 2dsphere v4 regression recurs, a secondary
 * crashes and the CheckReplDBHash suite hook detects the failure.
 *
 * @tags: [
 *   runs_set_fcv,
 *   # setFCV and the 2dsphere v4 oplog-apply fix require all nodes on the latest binary.
 *   multiversion_incompatible,
 *   # Regression detection relies on CheckReplDBHash across secondaries.
 *   requires_replication,
 *   # Even-tid threads take the two-phase async index build path.
 *   creates_background_indexes,
 * ]
 */

import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";

export const $config = (function () {
    // Each createIndex/dropIndex iteration picks one of these specs at random. The 2dsphere
    // entry is the primary regression target; the others broaden coverage to other index
    // types and compound combinations that may someday encounter FCV edge cases.
    //
    // Key-spec conflicts within this set are intentional where noted:
    //   - idx_text and idx_a_text: only one text index per collection is allowed; the second
    //     create attempt fails with CannotCreateIndex, which is tolerated.
    //   - idx_ab and idx_ab_partial: same key {a,b} but the partial filter makes them
    //     distinct, so both can coexist on the same collection.
    const kIndexSpecs = [
        // Special single-field types
        {key: {loc: "2dsphere"}, name: "idx_2dsphere"},
        {key: {loc2d: "2d"}, name: "idx_2d"},
        {key: {text_field: "text"}, name: "idx_text"},
        {key: {"$**": 1}, name: "idx_wildcard"},
        {key: {hash_field: "hashed"}, name: "idx_hashed"},
        // Plain single-field
        {key: {a: 1}, name: "idx_a"},
        {key: {b: -1}, name: "idx_b_desc"},
        // Compound — varying field counts and sort directions
        {key: {a: 1, b: 1}, name: "idx_ab"},
        {key: {a: -1, b: 1}, name: "idx_a_desc_b"},
        {key: {a: 1, b: 1, c: 1}, name: "idx_abc"},
        {key: {a: 1, b: 1, c: -1, d: 1}, name: "idx_abcd"},
        // Geo + regular field compounds
        {key: {loc: "2dsphere", a: 1}, name: "idx_2dsphere_a"},
        {key: {loc2d: "2d", a: 1}, name: "idx_2d_a"},
        // Compound with special types
        {key: {a: 1, text_field: "text"}, name: "idx_a_text"},
        {key: {hash_field: "hashed", b: 1}, name: "idx_hashed_b"},
        // Partial-filter compound (same key as idx_ab, coexists via distinct descriptor)
        {key: {a: 1, b: 1}, name: "idx_ab_partial", partialFilterExpression: {a: {$gt: 0}}},
    ];

    const states = {
        init: function (db, collName) {
            // Seed a document for even-tid threads so their createIndexes takes the two-phase
            // async build path, exercising ForwardableOperationMetadata OFCV propagation.
            if (this.tid % 2 === 0) {
                assert.commandWorked(
                    db[collName + "_" + this.tid].insert({
                        loc: {type: "Point", coordinates: [0, 0]},
                        loc2d: [0, 0],
                        text_field: "hello world",
                        hash_field: 42,
                        a: 1,
                        b: 2,
                        c: 3,
                        d: 4,
                    }),
                );
            }
        },

        createIndex: function (db, collName) {
            const spec = kIndexSpecs[Random.randInt(kIndexSpecs.length)];
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({
                    createIndexes: collName + "_" + this.tid,
                    indexes: [spec],
                }),
                [
                    ErrorCodes.CannotCreateIndex, // v4 spec rejected at downgraded FCV, or
                    // second text index on same collection
                    ErrorCodes.IndexAlreadyExists,
                    11332800, // disaggregated persistence provider requires primary-driven builds;
                    // two-phase (async) builds are rejected there
                ],
            );
        },

        dropIndex: function (db, collName) {
            const spec = kIndexSpecs[Random.randInt(kIndexSpecs.length)];
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({dropIndexes: collName + "_" + this.tid, index: spec.name}),
                [ErrorCodes.NamespaceNotFound, ErrorCodes.IndexNotFound],
            );
        },

        setFCV: function (db, collName) {
            const targetFCV = Random.rand() < 0.5 ? latestFCV : lastLTSFCV;
            try {
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
                );
            } catch (e) {
                if (handleRandomSetFCVErrors(e, targetFCV)) return;
                // CannotDowngrade: a concurrent createIndex added a v4 2dsphere index between
                // setFCV's first dry-run (which passed) and kPrepare's second dry-run. Not a
                // bug. FCV may be left in a transitional downgrading state; teardown restores
                // latestFCV.
                if (e.code === ErrorCodes.CannotDowngrade) return;
                // ConflictingOperationInProgress: the FCV command is blocked when a replica set
                // is being added to the sharded cluster. Expected in config-transitions suites.
                if (e.code === ErrorCodes.ConflictingOperationInProgress) return;
                throw e;
            }
        },
    };

    const transitions = {
        init: {createIndex: 1.0},
        createIndex: {createIndex: 0.2, dropIndex: 0.6, setFCV: 0.2},
        dropIndex: {createIndex: 0.6, dropIndex: 0.1, setFCV: 0.3},
        setFCV: {createIndex: 0.5, dropIndex: 0.3, setFCV: 0.2},
    };

    function setup(db, collName, cluster) {}

    function teardown(db, collName, cluster) {
        // Drop all per-thread collections to remove any v4 2dsphere indexes that could block
        // the FCV restore. CannotDowngrade during the workload can leave FCV in a transitional
        // downgrading state; clearing the blocking indexes lets the upgrade succeed.
        try {
            db.dropDatabase();
        } finally {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            );
        }
    }

    return {
        threadCount: 10,
        iterations: 1000,
        data: {},
        states: states,
        startState: "init",
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
