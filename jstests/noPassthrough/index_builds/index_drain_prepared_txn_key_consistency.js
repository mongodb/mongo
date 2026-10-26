/**
 * Runs an index build concurrently with a prepared transaction that changes the same document
 * multiple times.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("index key consistency when a prepared transaction commits during draining", function () {
    let rst, primary, coll;

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        coll = primary.getDB(jsTestName()).coll;
    });

    after(function () {
        rst.stopSet();
    });

    it("does not retain an intermediate key after transaction commit", function () {
        assert.commandWorked(coll.insert({_id: 0, a: -1}));
        const bulk = configureFailPoint(primary, "hangAfterIndexBuildDumpsInsertsFromBulk");

        const awaitBuild = IndexBuildTest.startIndexBuild(
            primary,
            coll.getFullName(),
            {a: 1},
            {},
            [],
            // Use majority commit quorum so that the drain uses an untimestamped read, which means
            // that the commit can change visibility within its snapshot.
            "majority",
        );
        bulk.wait();

        const session = primary.startSession();
        const sessionColl = session
            .getDatabase(coll.getDB().getName())
            .getCollection(coll.getName());
        session.startTransaction();

        // Write to `a` twice within the same document in a single transaction `T`, with unrelated
        // write `U` in between. All of these generate side writes for the index build.
        assert.commandWorked(sessionColl.insert({_id: 1, a: 640}));
        assert.commandWorked(coll.insert({_id: 2, a: 7}));
        assert.commandWorked(sessionColl.update({_id: 1}, {$set: {a: 0}}));
        const timestamp = PrepareHelpers.prepareTransaction(session);
        rst.awaitLastOpCommitted();

        const drain = configureFailPoint(primary, "hangIndexBuildDuringDrainWritesPhase", {
            iteration: 0,
            indexNames: ["a_1"],
        });
        bulk.off();

        // An unsafe drain skips T's first record and pauses at U. A correct drain instead stops
        // at T's prepare conflict. Resolve T in either case, not only with the fixed algorithm.
        assert.soon(() => {
            if (drain.waitWithTimeout(100)) {
                return true;
            }
            return coll
                .getDB()
                .currentOp({
                    $all: true,
                    "command.createIndexes": coll.getName(),
                    desc: {$regex: /IndexBuildsCoordinatorMongod/},
                })
                .inprog.some(
                    (op) => op.prepareReadConflicts > 0 && Object.keys(op.locks || {}).length === 0,
                );
        });

        assert.commandWorked(PrepareHelpers.commitTransaction(session, timestamp));
        drain.off();
        awaitBuild();

        session.endSession();
    });
});
