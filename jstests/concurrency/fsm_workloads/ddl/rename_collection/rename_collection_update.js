/**
 * rename_collection_update.js
 *
 * SERVER-101428 regression: cross-database renameCollection concurrently with
 * findAndModify updates on the source collection.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   incompatible_with_concurrency_simultaneous,
 *   multiversion_incompatible,
 *   requires_non_retryable_commands,
 *   requires_replication,
 *   uses_rename,
 * ]
 */
export const $config = (function () {
    const threadCount = 3;
    const iterations = 50;

    const data = {
        lostCollName: jsTestName() + "_lost",
        totalsCollName: jsTestName() + "_totals",
    };

    function toLostWritesTargetDBName(db) {
        return db.getName() + "_to";
    }

    function sumUpdates(coll) {
        if (coll.countDocuments({}) === 0) {
            return 0;
        }
        const agg = coll.aggregate([{$group: {_id: null, updates: {$sum: "$updates"}}}]).toArray();
        return agg.length ? agg[0].updates : 0;
    }

    function doOneWayRenameForLostWrites(db, lostCollName) {
        const targetDBName = toLostWritesTargetDBName(db);
        const renameCommand = {
            renameCollection: db.getName() + "." + lostCollName,
            to: targetDBName + "." + lostCollName,
            dropTarget: true,
        };
        assert.commandWorked(db.adminCommand(renameCommand));

        assert.commandWorked(db[lostCollName].insertOne({_id: 0, updates: 0}));

        const targetUpdates = sumUpdates(db.getSiblingDB(targetDBName)[lostCollName]);
        assert.commandWorked(
            db
                .getCollection(this.totalsCollName)
                .updateOne({}, {$inc: {updates: targetUpdates}}, {upsert: true}),
        );
    }

    function doUpdateForLostWrites(db, lostCollName) {
        // Retry on QueryPlanKilled: the findAndModify may yield mid-execution, allowing the rename to drop the source and kill the plan.
        let result;
        do {
            result = db.runCommand({
                findAndModify: lostCollName,
                query: {_id: this.tid},
                update: {$inc: {updates: 1}},
                upsert: true,
            });
        } while (!result.ok && result.code === ErrorCodes.QueryPlanKilled);
        assert.commandWorked(result);
    }

    const states = {
        loop: function (db, collName) {
            if (this.tid === 0) {
                doOneWayRenameForLostWrites.call(this, db, this.lostCollName);
            } else {
                doUpdateForLostWrites.call(this, db, this.lostCollName);
            }
        },
    };

    function setup(db, collName, cluster) {
        assert.commandWorked(db.createCollection(data.lostCollName));
        assert.commandWorked(db[data.lostCollName].insertOne({_id: 0, updates: 0}));
    }

    function teardown(db, collName, cluster) {
        let totalUpdates = sumUpdates(db.getCollection(this.totalsCollName));
        totalUpdates += sumUpdates(db[this.lostCollName]);

        const writerCount = this.threadCount - 1;
        const expectedUpdates = writerCount * this.iterations;
        assert.eq(expectedUpdates, totalUpdates, "Missing some updates", {
            expectedUpdates,
            totalUpdates,
        });
    }

    return {
        threadCount,
        iterations,
        data,
        states,
        startState: "loop",
        transitions: {loop: {loop: 1}},
        setup,
        teardown,
    };
})();
