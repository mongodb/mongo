/**
 * Verifies join optimization and join plan cache behavior when schema types change.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertJoinPlanCacheStats,
    joinOptUsed,
    joinTestWrapper,
} from "jstests/libs/query/join_utils.js";

describe("join schema type invalidation", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagQueryTypeInference: true,
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
                internalQueryExecYieldIterations: -1,
                internalQueryExecYieldPeriodMS: 3600000,
            },
        });
        this.db = this.conn.getDB(jsTestName());
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    beforeEach(function () {
        const db = this.db;
        const baseColl = db.base;
        const foreignColl = db.foreign;
        baseColl.drop();
        foreignColl.drop();
        assert.commandWorked(db.adminCommand({clearJoinPlanCache: 1}));
        // Base and foreign both have {a: 1} and {a: 2} and a schema showing 'a' is number,
        // and therefore non-array. Neither collection has an index on 'a'.
        for (const coll of [baseColl, foreignColl]) {
            assert.commandWorked(
                db.createCollection(coll.getName(), {
                    validator: {$jsonSchema: {properties: {a: {bsonType: "number"}}}},
                    validationLevel: "constraint",
                    validationAction: "error",
                }),
            );
            assert.commandWorked(
                coll.insertMany([
                    {_id: 1, a: 1},
                    {_id: 2, a: 2},
                ]),
            );
        }

        const pipeline = [
            {
                $lookup: {
                    from: foreignColl.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "f",
                },
            },
            {$unwind: "$f"},
        ];
        this.pipeline = pipeline;
        this.runQuery = () => {
            assert.sameMembers(baseColl.aggregate(pipeline).toArray(), [
                {_id: 1, a: 1, f: {_id: 1, a: 1}},
                {_id: 2, a: 2, f: {_id: 2, a: 2}},
            ]);
        };
    });

    /**
     * When the collection validator (and therefore inferred schema type) changes, the query
     * simply becomes ineligible to JO. The JO cache is not used in any way and is not invalidated.
     */
    it("does not use a cached join plan after schema is changed", function () {
        const db = this.db;
        joinTestWrapper(db, () => {
            const explain = db.base.explain().aggregate(this.pipeline);
            assert(joinOptUsed(explain), "expected join optimization", {explain});

            // We first have a cache miss, then a cache hit on the second run.
            assertJoinPlanCacheStats({db, fn: this.runQuery, expectedHits: 0, expectedMisses: 1});
            assertJoinPlanCacheStats({db, fn: this.runQuery, expectedHits: 1, expectedMisses: 0});
            const entries = db
                .getSiblingDB("admin")
                .aggregate([{$joinPlanCacheStats: {}}])
                .toArray();
            assert.eq(1, entries.length, "expected one cached join plan", {entries});

            // When we lower the validationLevel from 'constraint', we lose the schema type, since
            // lower levels do not guarantee all documents conform.
            assert.commandWorked(db.runCommand({collMod: "foreign", validationLevel: "strict"}));
            // So we expect that we can no longer use JO, since 'a' might be array.
            const noJoinExplain = db.base.explain().aggregate(this.pipeline);
            assert(
                !joinOptUsed(noJoinExplain),
                "expected no join optimization after schema change",
                {explain: noJoinExplain},
            );
            // And there's no cache hits or misses since we never reach that far.
            assertJoinPlanCacheStats({db, fn: this.runQuery, expectedHits: 0, expectedMisses: 0});
            // The JO cache entry remains, since we do not invalidate it.
            assert.sameMembers(
                db
                    .getSiblingDB("admin")
                    .aggregate([{$joinPlanCacheStats: {}}])
                    .toArray(),
                entries,
                "expected the join plan cache to remain",
            );
        });
    });

    /**
     * When the collection validator (and therefore inferred schema type) changes while a
     * dependent JO query has yielded, the query is killed, but the JO cache is kept.
     */
    it("invalidates JO query when schema changes", function () {
        const db = this.db;
        joinTestWrapper(db, () => {
            assertJoinPlanCacheStats({db, fn: this.runQuery, expectedHits: 0, expectedMisses: 1});

            // The second run will use the JO cache, but we will yield the run and show
            // the query is killed.
            let cursor;
            assertJoinPlanCacheStats({
                db,
                fn: () => {
                    cursor = assert.commandWorked(
                        db.runCommand({
                            aggregate: "base",
                            pipeline: this.pipeline,
                            cursor: {batchSize: 1},
                        }),
                    ).cursor;
                },
                expectedHits: 1,
                expectedMisses: 0,
            });

            assert.neq(NumberLong(0), cursor.id, "expected a cursor", {cursor});
            assert.eq(1, cursor.firstBatch.length, "expected one result in the first batch", {
                cursor,
            });
            const entries = db
                .getSiblingDB("admin")
                .aggregate([{$joinPlanCacheStats: {}}])
                .toArray();
            assert.eq(1, entries.length, "expected one cached join plan", {entries});

            assert.commandWorked(db.runCommand({collMod: "foreign", validationLevel: "strict"}));
            const response = db.runCommand({getMore: cursor.id, collection: "base"});
            assert.commandFailedWithCode(response, ErrorCodes.QueryPlanKilled);
            assert(
                response.errmsg.includes("collection schema metadata changed during yield"),
                "expected query invalidation",
                {response},
            );
            assert.sameMembers(
                db
                    .getSiblingDB("admin")
                    .aggregate([{$joinPlanCacheStats: {}}])
                    .toArray(),
                entries,
                "expected the join plan cache to remain",
            );
        });
    });
});
