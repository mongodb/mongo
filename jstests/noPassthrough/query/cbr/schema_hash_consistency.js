/**
 * Regression tests for tripwire 11763501 "An expected plan was not found" in MultiPlanStage::abandonTrialsExceptHashes, hit by the mixed plan ranker's CBRForNoMPResultsStrategy when the plan it picks on re-enumeration hashes differently from the original trial candidates. The tests drive the same code path with queries whose match expressions are affected by order-dependent hashing and assert that planning completes and returns the correct results.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("CBR plan hash consistency", function () {
    let conn;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagCostBasedRanker: true,
                internalQueryPlanRanker: "mixed",
                internalQueryFrameworkControl: "forceClassicEngine",
                internalQueryPlanEvaluationWorks: 1,
            },
        });
        assert.neq(null, conn);
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Two $jsonSchema predicates whose nested 'additionalProperties'/'properties' constraints are equivalent, so the re-enumerated plan contains an equivalent-but-differently
    // constructed InternalSchemaAllowedPropertiesMatchExpression. The queries match no documents, so multi-planning trials produce no results and CBR re-enumerates and picks a winner.
    it("hashes equivalent schema expressions consistently across re-enumeration", function () {
        const coll = conn.getDB(jsTestName()).coll;
        assert.commandWorked(coll.insertMany(Array.from({length: 100}, () => ({a: 1, b: 1}))));
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));

        const schema = (values) => ({
            bsonType: "object",
            required: ["a"],
            properties: {
                a: {bsonType: "number", enum: values},
                nested: {
                    additionalProperties: false,
                    properties: Object.fromEntries(
                        Array.from({length: 20}, (_, i) => ["field" + i, {}]),
                    ),
                },
            },
        });
        const filter = {
            a: 1,
            b: 1,
            $and: [{$jsonSchema: schema([1])}, {$nor: [{$jsonSchema: schema([1, 2])}]}],
        };

        // Replan repeatedly to exercise different construction orders of the equivalent
        // predicates across enumerations.
        for (let i = 0; i < 20; ++i) {
            coll.getPlanCache().clear();
            assert.eq(0, coll.find(filter, {_id: 1}).limit(5).itcount());
        }
    });

    // A $in over uuids where some values exist but the capped trials find no results, so CBR re-enumerates and must keep the trial candidate matching its chosen plan.
    it("hashes the re-enumerated $in plan consistently with the trial candidates", function () {
        const coll = conn.getDB(jsTestName()).talents;
        assert.commandWorked(coll.createIndex({uuid: 1}));
        assert.commandWorked(coll.createIndex({uuid: 1, org: 1}));

        // Document uuids are disjoint from the queried ones except for two values at the end of the $in list so the capped trials find no results and we use CBR.
        const docUuids = Array.from({length: 200}, () => UUID());
        const queried = Array.from({length: 50}, () => UUID());
        docUuids[7] = queried[45];
        docUuids[42] = queried[47];
        assert.commandWorked(coll.insertMany(docUuids.map((u, i) => ({uuid: u, org: i % 7}))));

        for (let i = 0; i < 50; ++i) {
            coll.getPlanCache().clear();
            assert.eq(
                2,
                coll
                    .find({uuid: {$in: queried}}, {_id: 1})
                    .limit(5)
                    .itcount(),
            );
        }
    });
});
