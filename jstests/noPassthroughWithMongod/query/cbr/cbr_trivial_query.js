import {getCBRConfig, setCBRConfig} from "jstests/libs/query/cbr_utils.js";

const coll = db[jsTestName()];

function triviallyTrue() {
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.insertOne({}));

    const queries = [
        () => coll.find().limit(5),
        () => coll.find().sort({a: 1}),
        () => coll.find().sort({b: 1}),
        () => coll.find().sort({a: 1}).limit(5),
        () => coll.find().sort({b: 1}).limit(5),
    ];

    for (const query of queries) {
        const winningPlan = query().explain().queryPlanner.winningPlan;
        if ("estimatesMetadata" in winningPlan) {
            assert.eq(winningPlan.estimatesMetadata.ceSource, "Metadata");
            assert.eq(winningPlan.cardinalityEstimate, 1);
        }
        const res = query().toArray();
        assert.eq(res.length, 1);
    }
}

function triviallyFalse() {
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.insertOne({}));

    const winningPlan = coll.find({$expr: false}).explain().queryPlanner.winningPlan;
    if ("estimatesMetadata" in winningPlan) {
        assert.eq(winningPlan.estimatesMetadata.ceSource, "Code");
        assert.eq(winningPlan.cardinalityEstimate, 0);
    }
    const res = coll.find({$expr: false}).toArray();
    assert.eq(res.length, 0);
}

function multiKey() {
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.insertOne({a: [1, 2]}));

    const query = () => coll.find().sort({a: 1});
    const winningPlan = query().explain("executionStats").queryPlanner.winningPlan;
    if ("estimatesMetadata" in winningPlan) {
        assert.eq(winningPlan.estimatesMetadata.ceSource, "Sampling");
        assert.eq(winningPlan.cardinalityEstimate, 1);
        assert.eq(winningPlan.stage, "FETCH");
        assert.eq(winningPlan.inputStage.numKeysEstimate, 2);
        assert.eq(winningPlan.inputStage.cardinalityEstimate, 1);
    }
    const res = query().toArray();
    assert.eq(res.length, 1);
}

const oldCBRConfig = getCBRConfig(db);

try {
    for (const mode of ["automaticCE", "samplingCE"]) {
        assert.commandWorked(
            db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: true, internalQueryCBRCEMode: mode}),
        );

        triviallyTrue();
        triviallyFalse();
        multiKey();
    }
} finally {
    setCBRConfig(db, oldCBRConfig);
}
