/**
 * Verifies 'internalQuerySamplingCEMethodForPersistentSamples' and 'internalQuerySamplingCEMethod'
 * are independent knobs. First is for the type of persisted sample to load and the
 * latter is for the on-the-fly fallback technique. See also cbr_persistent_sample.js.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagCostBasedRanker: true,
        featureFlagPersistentStats: true,
        internalQueryCBRCEMode: "samplingCE",
        internalQueryDisablePlanCache: 1,
        internalQuerySamplingBySequentialScan: false,
        internalQuerySamplingByStrides: false,
    },
});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(jsTestName());
const kCollName = jsTestName();

// TODO SERVER-112627: Remove once featureFlagPersistentStats is enabled by default.
if (!FeatureFlagUtil.isEnabled(db, "PersistentStats")) {
    jsTest.log.info(`Skipping ${jsTestName()}: featureFlagPersistentStats is not enabled`);
    MongoRunner.stopMongod(conn);
    quit();
}

const coll = db[kCollName];
const kSampleSize = PersistentSamplesUtils.defaultSampleSize(db);
const kNumDocs = 2 * kSampleSize;

// Sets both sampling-method knobs.
function setMethodKnobs(persistedMethod, onTheFlyMethod) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQuerySamplingCEMethodForPersistentSamples: persistedMethod,
            internalQuerySamplingCEMethod: onTheFlyMethod,
        }),
    );
}

// Persists a sample of the given method using the read path's default sample size.
function persistSample(method) {
    assert.commandWorked(
        db.runCommand({
            analyze: kCollName,
            mode: "sample",
            sampleSize: kSampleSize,
            samplingMethod: method,
        }),
    );
}

// Returns the ceSamplingMetadata entry for the collection from an explain of a sampling-CE query.
function getSamplingMeta() {
    const explain = coll.find({a: {$gte: 0}, b: {$gte: 0}}).explain();

    const meta = explain.queryPlanner.ceSamplingMetadata?.[coll.getFullName()];
    assert(meta, "Expected a ceSamplingMetadata entry (sampling CE did not run?)", {
        explain,
    });
    return meta;
}

describe("persistent-sample read path: persisted-method and on-the-fly-method knobs", function () {
    before(function () {
        const docs = Array.from({length: kNumDocs}, (_, i) => ({_id: i, a: i, b: i % 10}));
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Each case sets both knobs, optionally persists a sample, then checks the technique used.
    const cases = [
        {
            name: "loads a random persisted sample when persisted-method=random",
            persistedMethod: "random",
            onTheFlyMethod: "chunk",
            persist: "random",
            expectSource: "persisted",
            expectTechnique: "random",
        },
        {
            name: "loads a chunk persisted sample when persisted-method=chunk",
            persistedMethod: "chunk",
            onTheFlyMethod: "random",
            persist: "chunk",
            expectSource: "persisted",
            expectTechnique: "chunk",
        },
        {
            name: "ignores a chunk persisted sample when persisted-method=random (miss -> OTF)",
            persistedMethod: "random",
            onTheFlyMethod: "chunk",
            persist: "chunk",
            expectSource: "onTheFly",
            expectTechnique: "chunk",
        },
        {
            name: "ignores a random persisted sample when persisted-method=chunk (miss -> OTF)",
            persistedMethod: "chunk",
            onTheFlyMethod: "random",
            persist: "random",
            expectSource: "onTheFly",
            expectTechnique: "random",
        },
    ];

    for (const tc of cases) {
        it(tc.name, function () {
            PersistentSamplesUtils.dropSamplesColl(db);
            setMethodKnobs(tc.persistedMethod, tc.onTheFlyMethod);
            if (tc.persist) {
                persistSample(tc.persist);
            }

            const meta = getSamplingMeta();
            assert.eq(meta.sampleSource, tc.expectSource, "unexpected sampleSource", {tc, meta});
            assert.eq(meta.sampleTechnique, tc.expectTechnique, "unexpected sampleTechnique", {
                tc,
                meta,
            });
        });
    }
});
