/**
 * Test featureFlagSamplingCEKeystringEstimation.
 * Wildcard/hashed/collated indices must be inestimable by CBR if it is disabled.
 */

import {getRejectedPlans, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {
    assertPlanCosted,
    assertPlanNotCosted,
    getPlanRankerConfig,
    setPlanRankerConfig,
} from "jstests/libs/query/cbr_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];

const config = getPlanRankerConfig(db);
// The ranker-config helpers do not manage this flag: restore it manually.
const keystringFlagBefore = db.adminCommand({
    getParameter: 1,
    featureFlagSamplingCEKeystringEstimation: 1,
}).featureFlagSamplingCEKeystringEstimation.value;

describe("featureFlagSamplingCEKeystringEstimation", function () {
    before(function () {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                featureFlagCostBasedRanker: true,
                internalQueryPlanRanker: "costBased",
                internalQueryCBRCEMode: "samplingCE",
            }),
        );

        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1}, {name: "normal"}));
        assert.commandWorked(
            coll.createIndex({a: 1}, {name: "collated", collation: {locale: "en", strength: 1}}),
        );
        assert.commandWorked(coll.createIndex({a: "hashed"}, {name: "hashed"}));
        assert.commandWorked(coll.createIndex({"$**": 1}, {name: "wildcard"}));
    });

    after(function () {
        setPlanRankerConfig(db, config);
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                featureFlagSamplingCEKeystringEstimation: keystringFlagBefore,
            }),
        );
    });

    it("costs wildcard/hashed/collated index scans iff the flag is enabled", function () {
        for (const flag of [false, true]) {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, featureFlagSamplingCEKeystringEstimation: flag}),
            );
            for (const index of ["collated", "hashed", "wildcard"]) {
                const explain = coll.find({a: 0}).hint(index).explain();
                const plans = [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)];
                assert.eq(plans.length, 1, explain);
                // Hashed/wildcard/collated index scans are costed if and only if the flag is enabled.
                plans.forEach(flag ? assertPlanCosted : assertPlanNotCosted);
            }
        }
    });

    it("costs normal indices even with the flag disabled", function () {
        for (const flag of [false, true]) {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, featureFlagSamplingCEKeystringEstimation: flag}),
            );
            const explain = coll.find({a: 0}).hint("normal").explain();
            const plans = [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)];
            assert.eq(plans.length, 1, explain);
            // Normal indices can be costed even with the flag disabled.
            plans.forEach(assertPlanCosted);
        }
    });
});
