/*
 * Tests that when the data consistency checker cannot run a command it needs in order to compare
 * the primary and a secondary, it reports a command failure rather than the generic dbhash mismatch
 * error. Reporting a command failure as a dbhash mismatch produces false-positive data
 * inconsistency classifications (see SERVER-132466).
 *
 * @tags: [uses_testing_only_commands]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "db0";
const collName = "testColl";
const ns = `${dbName}.${collName}`;
const errorCode = ErrorCodes.OperationFailed;

const kMismatchMsg = "dbhash mismatch between primary and secondary";
const kCommandFailureMsg = "data consistency check command failure";

describe("checkReplicatedDataHashes error classification", function () {
    before(function () {
        // The checks below intentionally leave the nodes inconsistent, so the check in stopSet()
        // would fail.
        this.originalSkipCheckDBHashes = TestData.skipCheckDBHashes;
        TestData.skipCheckDBHashes = true;

        this.rst = new ReplSetTest({name: jsTestName(), nodes: 2});
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.secondary = this.rst.getSecondary();

        // The default write concern is majority, and the godinsert command used below to create an
        // inconsistency runs on a secondary, which is incompatible with w:majority.
        assert.commandWorked(
            this.primary.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1},
                writeConcern: {w: "majority"},
            }),
        );

        assert.commandWorked(
            this.primary.getDB(dbName).runCommand({insert: collName, documents: [{_id: 0}]}),
        );
        this.rst.awaitReplication();

        // Fails collStats on 'node' for the test namespace only, so that the reported failure list
        // stays small.
        this.failCollStatsOn = (node) =>
            configureFailPoint(node, "failCommand", {
                errorCode: errorCode,
                failCommands: ["collStats"],
                namespace: ns,
            });

        // Makes the secondary disagree with the primary by writing only to the secondary. Each call
        // writes a new document so that callers do not depend on each other.
        let nextInconsistentId = 1;
        this.createInconsistency = () =>
            assert.commandWorked(
                this.secondary
                    .getDB(dbName)
                    .runCommand({godinsert: collName, obj: {_id: nextInconsistentId++}}),
            );
    });

    after(function () {
        this.rst.stopSet();
        TestData.skipCheckDBHashes = this.originalSkipCheckDBHashes;
    });

    it("passes when both nodes are reachable and their data matches", function () {
        this.rst.checkReplicatedDataHashes();
    });

    // A failure to run collStats means the comparison could not be made at all, so it must not be
    // reported as a dbhash mismatch, on whichever node it happens.
    for (const role of ["primary", "secondary"]) {
        it(`reports a collStats failure on the ${role} as a command failure`, function () {
            const fp = this.failCollStatsOn(this[role]);
            try {
                const err = assert.throws(() => this.rst.checkReplicatedDataHashes());
                assert(
                    err.message.includes(kCommandFailureMsg),
                    `collStats failing on the ${role} was not reported as a command failure`,
                    {err},
                );
                assert(
                    !err.message.includes(kMismatchMsg),
                    `collStats failing on the ${role} was reported as a dbhash mismatch`,
                    {err},
                );

                // The underlying error must be preserved so the real problem is not masked.
                const failures = err.extraAttr.commandFailures;
                assert.eq(1, failures.length, "expected exactly one command failure", {failures});
                const failure = failures[0];
                assert.eq("collStats", failure.command, "wrong command reported", {failure});
                assert.eq(role, failure.role, "wrong node role reported", {failure});
                assert.eq(ns, failure.ns, "wrong namespace reported", {failure});
                assert.eq(this[role].host, failure.host, "wrong host reported", {failure});
                assert.eq(errorCode, failure.code, "error code not preserved", {failure});
                assert.eq("OperationFailed", failure.codeName, "codeName not preserved", {failure});
                assert(failure.errmsg.includes("failCommand"), "errmsg not preserved", {failure});
            } finally {
                fp.off();
            }
        });
    }

    it("reports a genuine inconsistency as a dbhash mismatch", function () {
        this.createInconsistency();

        const err = assert.throws(() => this.rst.checkReplicatedDataHashes());
        assert(err.message.includes(kMismatchMsg), "expected a dbhash mismatch", {err});
    });

    it("reports a genuine inconsistency as a dbhash mismatch even when a command also failed", function () {
        // A command failure must never hide a real data inconsistency.
        this.createInconsistency();

        const fp = this.failCollStatsOn(this.primary);
        try {
            const err = assert.throws(() => this.rst.checkReplicatedDataHashes());
            assert(err.message.includes(kMismatchMsg), "expected a dbhash mismatch", {err});
        } finally {
            fp.off();
        }
    });
});
