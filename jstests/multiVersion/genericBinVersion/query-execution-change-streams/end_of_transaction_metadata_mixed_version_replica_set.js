/**
 * Verifies that a multi-document transaction committed on a mixed-version replica set replicates to
 * the other node, in both directions:
 *
 *  - committed on a latest primary and replicated to a last-LTS secondary;
 *  - committed on a last-LTS primary and replicated to a latest secondary.
 *
 * In both cases the transaction must commit, and its documents must be visible on both the primary
 * and the secondary afterwards.
 *
 * @tags: [
 *   uses_transactions,
 *   requires_persistence,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";

const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;

// Starts a 2-node replica set whose node 0 runs 'primaryBinVersion' and node 1 runs
// 'secondaryBinVersion'. Node 0 is given the higher priority so that it stays the primary.
function startMixedVersionReplSet(primaryBinVersion, secondaryBinVersion) {
    const rst = new ReplSetTest({
        nodes: [
            {binVersion: primaryBinVersion, rsConfig: {priority: 2}},
            {binVersion: secondaryBinVersion, rsConfig: {priority: 1}},
        ],
    });
    rst.startSet();
    rst.initiate();
    assert.eq(rst.nodes[0].host, rst.getPrimary().host, "expected node 0 to be the primary");
    return rst;
}

// Commits a transaction inserting 'docs' on the primary of 'rst', then asserts that the documents
// are visible on both the primary and the secondary.
function commitTransactionAndAssertVisibleOnBothNodes(rst, docs) {
    const primary = rst.getPrimary();
    withSession(primary, (session) => {
        const sessionColl = session.getDatabase(dbName)[collName];
        session.startTransaction();
        for (const doc of docs) {
            assert.commandWorked(sessionColl.insert(doc));
        }
        assert.commandWorked(session.commitTransaction_forTesting());
    });

    const secondary = rst.getSecondary();
    for (const node of [primary, secondary]) {
        for (const doc of docs) {
            assert.soon(
                () => node.getCollection(ns).findOne(doc) !== null,
                "expected the committed document to be visible",
                undefined,
                undefined,
                undefined,
                {node: node.host, doc},
            );
        }
    }
}

describe("commit on a latest primary, replicated to a last-LTS secondary", function () {
    let rst;

    before(function () {
        rst = startMixedVersionReplSet("latest", "last-lts");
    });

    after(function () {
        rst.stopSet();
    });

    it("commits the transaction and replicates it to the last-LTS secondary", function () {
        commitTransactionAndAssertVisibleOnBothNodes(rst, [{_id: 1}, {_id: 2}]);
    });
});

describe("commit on a last-LTS primary, replicated to a latest secondary", function () {
    let rst;

    before(function () {
        rst = startMixedVersionReplSet("last-lts", "latest");
    });

    after(function () {
        rst.stopSet();
    });

    it("commits the transaction and replicates it to the latest secondary", function () {
        commitTransactionAndAssertVisibleOnBothNodes(rst, [{_id: 1}, {_id: 2}]);
    });
});
