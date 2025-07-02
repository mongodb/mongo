/**
 * Runs rawData CRUD operations and explains against time-series collections while dropping and
 * re-creating them.
 *
 * @tags: [
 *  requires_timeseries,
 *  requires_fcv_82,
 *  # Time-series collections cannot be written to in a transaction.
 *  does_not_support_transactions,
 *  # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *  does_not_support_stepdowns,
 *  requires_getmore,
 * ]
 */

import {getPlanStage} from "jstests/libs/query/analyze_plan.js";

export const $config = (function() {
    // This value must be 10. The tids of the threads are sequential, but don't necessarily start at
    // 0. 10 threads means that for any tid offset, exactly 1 thread has a tid ending in 1. This
    // allows threads to know which group and role they are.
    const threadCount = 10;
    const metaFieldName = "meta";
    const timeFieldName = "time";
    const measurementsPerBucket = 2;
    const bucketsPerCollection = 5;

    const acceptableErrorCodes = [
        // Encountered if a collection is dropped while a command is running.
        ErrorCodes.QueryPlanKilled,
        // Encountered if a command runs while the collection transiently doesn't exist.
        ErrorCodes.NamespaceNotFound,
        // Encountered if the collection is created very shortly after running a command.
        // TODO SERVER-103845: This race condition will not be possible once viewless time-series is
        // complete.
        ErrorCodes.CommandNotSupportedOnView,
        // Encountered (especially in slower suites) if the router retries the operation too many
        // times due to transient errors.
        ErrorCodes.StaleConfig,
    ];

    const data = {
        // The test is broken up into 5 groups of 2 threads, each group having its own collection.
        // Within a group, both threads run CRUD operations on their group's collection, and one of
        // the threads also drops and re-creates the collection. This prevents collection creation
        // from racing with itself.
        getCollectionName: function() {
            return jsTestName() + "_" + Math.floor(this.tid % threadCount / 2);
        },

        getCollection: function(db) {
            return db.getCollection(this.getCollectionName());
        },

        recreateCollection: function(db) {
            if (this.tid % 2 == 0) {
                return;
            }

            assert(this.getCollection(db).drop());
            assert.commandWorked(db.createCollection(
                this.getCollectionName(),
                {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

            const bulk = this.getCollection(db).initializeOrderedBulkOp();
            for (let i = 0; i < bucketsPerCollection; i++) {
                for (let j = 0; j < measurementsPerBucket; j++) {
                    bulk.insert({
                        [timeFieldName]: new Date(),
                        [metaFieldName]: i,
                        data: j,
                    });
                }
            }
            assert.commandWorked(bulk.execute());
        },

        assertExplain: function(db, commandResult, commandName) {
            const coll = this.getCollection(db);
            assert(commandResult.ok);
            if (commandResult.command.pipeline) {
                for (const stage of commandResult.command.pipeline) {
                    assert.isnull(stage["$_internalUnpackBucket"],
                                  `Expected not to find $_internalUnpackBucket stage`);
                }
            }
            assert.isnull(getPlanStage(commandResult, "TS_MODIFY")),
                "Expected not to find TS_MODIFY stage " + tojson(commandResult);
            assert(commandResult.command.rawData,
                   `Expected command to include rawData but got ${tojson(commandResult)}`);
            assert.eq(commandResult.command[commandName],
                      coll.getName(),
                      `Expected command namespace to be ${tojson(coll.getName())} but got ${
                          tojson(commandResult.command[commandName])}`);
        },

        handleCollectionDrop: function(fn, opName) {
            try {
                return fn();
            } catch (e) {
                if (acceptableErrorCodes.includes(e.code)) {
                    jsTestLog(
                        `${opName} failed due to race with collection drop (transient, this is expected).`);
                } else {
                    throw e;
                }
            }
        },
    };

    const states = {
        recreateCollection: function recreateCollection(db, collName) {
            this.recreateCollection(db);
        },

        update: function update(db, collName) {
            const coll = this.getCollection(db);
            assert.commandWorkedOrFailedWithCode(
                coll.update({"control.count": measurementsPerBucket},
                            {$set: {"control.closed": true}},
                            {rawData: true, multi: true}),
                acceptableErrorCodes);
        },

        count: function count(db, collName) {
            const coll = this.getCollection(db);
            this.handleCollectionDrop(() => {
                const res = coll.count({control: {$exists: true}}, {rawData: true});
                assert.lte(
                    res,
                    bucketsPerCollection,
                    `Expected between 0 and ${bucketsPerCollection}, got ${res}. (A value of ${
                        measurementsPerBucket *
                        bucketsPerCollection} suggests an issue with rawData)`);
            }, "Count");
        },

        find: function find(db, collName) {
            const coll = this.getCollection(db);
            this.handleCollectionDrop(() => {
                const res = coll.find({[metaFieldName]: 2}).rawData().itcount();
                assert(res == 0 || res == 1,
                       `Expected 0 or 1 buckets, got ${res}. (A value of ${
                           measurementsPerBucket} suggests an issue with rawData)`);
            }, "Find");
        },

        distinct: function distinct(db, collName) {
            const coll = this.getCollection(db);
            this.handleCollectionDrop(() => {
                const res = coll.distinct("data", {}, {rawData: true}).length;
                assert.lte(res,
                           bucketsPerCollection,
                           `Expected between 0 and ${bucketsPerCollection} buckets, got ${
                               res.length}. (A value of ${
                               measurementsPerBucket} suggests an issue with rawData)`);
            }, "Distinct");
        },

        aggregate: function aggregate(db, collName) {
            const coll = this.getCollection(db);
            this.handleCollectionDrop(() => {
                const res =
                    coll.aggregate([{$group: {_id: "$meta"}}], {rawData: true}).toArray().length;
                assert.lte(res,
                           bucketsPerCollection,
                           `Expected between 0 and ${bucketsPerCollection} buckets, got ${
                               res}. (A value of ${
                               measurementsPerBucket} suggests an issue with rawData)`);
            }, "Aggregate");
        },

        explainAggregate: function explainAggregate(db, collName) {
            const coll = this.getCollection(db);
            const result = this.handleCollectionDrop(
                () => coll.explain().aggregate([{$match: {[metaFieldName]: 1}}], {rawData: true}),
                "Explain aggregate");
            if (result) {
                this.assertExplain(db, result, "aggregate");
            }
        },

        explainCount: function explainCount(db, collName) {
            const coll = this.getCollection(db);
            const result = this.handleCollectionDrop(
                () => coll.explain().count({[metaFieldName]: 1}, {rawData: true}), "Explain count");
            if (result) {
                this.assertExplain(db, result, "count");
            }
        },

        explainDistinct: function explainDistinct(db, collName) {
            const coll = this.getCollection(db);
            const result = this.handleCollectionDrop(
                () => coll.explain().distinct(metaFieldName, {}, {rawData: true}),
                "Explain distinct");
            if (result) {
                this.assertExplain(db, result, "distinct");
            }
        },

        explainFind: function explainFind(db, collName) {
            const coll = this.getCollection(db);
            const result = this.handleCollectionDrop(
                () => coll.explain().find({[metaFieldName]: 1}).rawData().finish(), "Explain find");
            if (result) {
                this.assertExplain(db, result, "find");
            }
        },

        explainFindAndModify: function explainFindAndModify(db, collName) {
            const coll = this.getCollection(db);
            const result = this.handleCollectionDrop(() => coll.explain().findAndModify({
                query: {"control.count": measurementsPerBucket},
                update: {$set: {"control.closed": true}},
                rawData: true,
            }),
                                                     "Explain findAndModify");
            if (result) {
                this.assertExplain(db, result, "findAndModify");
            }
        },

        explainUpdate: function explainUpdate(db, collName) {
            const coll = this.getCollection(db);
            const result = this.handleCollectionDrop(
                () => coll.explain().update({"control.count": measurementsPerBucket},
                                            {$set: {"control.closed": true}},
                                            {rawData: true}),
                "Explain update");
            if (result) {
                this.assertExplain(db, result, "update");
            }
        },

        explainDelete: function explainDelete(db, collName) {
            const coll = this.getCollection(db);
            const result = this.handleCollectionDrop(
                () => coll.explain().remove({"control.count": measurementsPerBucket},
                                            {rawData: true}),
                "Explain delete");
            if (result) {
                this.assertExplain(db, result, "delete");
            }
        },
    };

    const keys = Object.keys(states);
    const transitions = Object.fromEntries(keys.map(
        k => [k, Object.fromEntries(keys.map(k2 => [k2, k2 === 'recreateCollection' ? 10 : 1]))]));

    return {
        threadCount: threadCount,
        iterations: 100,
        startState: "recreateCollection",
        data: data,
        states: states,
        transitions: transitions,
    };
})();
