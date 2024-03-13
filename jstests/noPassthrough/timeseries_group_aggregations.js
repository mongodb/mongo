/**
 * This test is meant to check correctness of block processing with a diverse set of queries that
 * would be hard to write out by hand.
 * We create a list of common pipeline stages that could run in block processing, and run different
 * permutations of them, while checking the results against a mongod using the classic engine.
 */

const classicConn =
    MongoRunner.runMongod({setParameter: {internalQueryFrameworkControl: "forceClassicEngine"}});
const bpConn = MongoRunner.runMongod(
    {setParameter: {featureFlagSbeFull: true, featureFlagTimeSeriesInSbe: true}});

assert.neq(null, classicConn, "mongod was unable to start up");
assert.neq(null, bpConn, "mongod was unable to start up");

// Only run this test for debug=off opt=on without sanitizers active, since this test runs lots of
// queries.
function isSlowBuild(db) {
    const debugBuild = db.adminCommand("buildInfo").debug;
    return debugBuild || !_optimizationsEnabled() || _isAddressSanitizerActive() ||
        _isLeakSanitizerActive() || _isThreadSanitizerActive() ||
        _isUndefinedBehaviorSanitizerActive();
}

const classicDb = classicConn.getDB(jsTestName());
const bpDb = bpConn.getDB(jsTestName());
if (isSlowBuild(classicDb) || isSlowBuild(bpDb)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    MongoRunner.stopMongod(classicConn);
    MongoRunner.stopMongod(bpConn);
    quit();
}

const classicColl = classicDb.timeseries_group_bson_types;
const bpColl = bpDb.timeseries_group_bson_types;

classicColl.drop();
bpColl.drop();
// Create a TS collection to get block processing running. Compare this against a classic
// collection.
assert.commandWorked(bpDb.createCollection(bpColl.getName(), {
    timeseries: {timeField: 't', metaField: 'm'},
}));

const datePrefix = 1680912440;
const dateLowerBound = new Date(datePrefix);

// Create time series buckets with different meta fields, time fields, and data.
const allFields = ['t', 'm', 'a', 'b'];
const tsData = [];
const alphabet = 'abcdefghijklmnopqrstuvwxyz';
for (let m = 0; m < 10; m++) {
    let currentDate = 0;
    for (let i = 0; i < 10; i++) {
        tsData.push(
            {t: new Date(datePrefix + currentDate - 100), m: m, a: 10 - i, b: alphabet.charAt(i)});
        currentDate += 25;
    }
}

classicColl.insert(tsData);
bpColl.insert(tsData);

function compareClassicAndBP(pipeline, allowDiskUse) {
    const classicResults = classicColl.aggregate(pipeline, {allowDiskUse}).toArray();
    const bpResults = bpColl.aggregate(pipeline, {allowDiskUse}).toArray();

    // Sort order is not guaranteed, so let's sort by the object itself before comparing.
    const cmpFn = function(doc1, doc2) {
        return tojson(doc1) < tojson(doc2);
    };
    classicResults.sort(cmpFn);
    bpResults.sort(cmpFn);
    assert.eq(classicResults, bpResults, {pipeline});
}

// Create $project stages.
const projectStages = [
    {stage: {$project: {t: '$t'}}, uses: ['t'], produces: ['t']},
    {stage: {$project: {m: '$m'}}, uses: ['m'], produces: ['m']},
    {stage: {$project: {a: '$a'}}, uses: ['a'], produces: ['a']}
];
for (const includeField of [0, 1]) {
    projectStages.push({
        stage: {$project: {t: includeField}},
        uses: ['t'],
        produces: includeField ? ['t'] : ['m', 'a', 'b']
    });
    projectStages.push({
        stage: {$project: {m: includeField}},
        uses: ['m'],
        produces: includeField ? ['m'] : ['t', 'a', 'b']
    });
    projectStages.push({
        stage: {$project: {a: includeField}},
        uses: ['a'],
        produces: includeField ? ['a'] : ['t', 'm', 'b']
    });
}

// Create $match stages.
const matchStages = [];
const matchComparisons = [
    {field: 't', comp: dateLowerBound},
    {field: 'm', comp: 5},
    {field: 'a', comp: 2},
];
for (const matchComp of matchComparisons) {
    for (const comparator of ['$lt', '$gt', '$lte', '$gte', '$eq']) {
        matchStages.push({
            stage: {$match: {[matchComp.field]: {[comparator]: matchComp.comp}}},
            uses: [matchComp.field],
            produces: allFields
        });
    }
}

// Create $group stages.
const groupStages = [
    {stage: {$count: 'c'}, uses: []},
    // Add some compound key cases since we don't want to try every combination.
    {stage: {$group: {_id: {t: '$t', m: '$m'}, gb: {$min: '$a'}}}, uses: ['t', 'm', 'a']},
    {stage: {$group: {_id: {a: '$a', m: '$m'}, gb: {$avg: '$t'}}}, uses: ['t', 'm', 'a']},
    // Date trunc example.
    {
        stage: {$group: {_id: {$dateTrunc: {date: "$t", unit: "hour"}}, gb: {$max: '$a'}}},
        uses: ['t', 'a']
    },
];

for (const groupKey of [null, 't', 'm', 'a']) {
    const dollarGroupKey = groupKey === null ? null : '$' + groupKey;
    for (const accumulatorData of ['t', 'm', 'a']) {
        const dollarAccumulatorData = accumulatorData === null ? null : '$' + accumulatorData;
        for (const accumulator of ['$min', '$max', '$sum']) {
            if (groupKey !== accumulatorData) {
                const uses = [accumulatorData];
                // "null" is not a field we need from previous stages.
                if (groupKey !== null) {
                    uses.push(groupKey);
                }
                groupStages.push({
                    stage:
                        {$group: {_id: dollarGroupKey, gb: {[accumulator]: dollarAccumulatorData}}},
                    uses: uses
                });
            }
        }
    }
    const uses = groupKey === null ? [] : [groupKey];
    groupStages.push({stage: {$group: {_id: dollarGroupKey, gb: {$count: {}}}}, uses: uses});
}

const projectMatchStages = [...projectStages, ...matchStages];

// Does stage1 provide what stage2 needs to execute?
function stageRequirementsMatch(stage1, stage2) {
    for (const req of stage2.uses) {
        if (!stage1.produces.includes(req)) {
            return false;
        }
    }
    return true;
}

function runAggregations(allowDiskUse, forceSpilling) {
    // Don't set the flags on classic because it's already considered correct.
    assert.commandWorked(bpDb.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling: forceSpilling
    }));
    /*
     * Try all combinations of project and match stages followed by a groupby at the end. To avoid
     * running unrealistic queries, we'll check what fields a stage needs to run. For example
     * a {$match: {m: 1}} needs the "m" field to do anything useful. If we have:
     *     [{$project: {a: 1}}, {$match: {m: 1}}]
     * the $project removes "m" so this is not a realistic query.
     * For this reason we mark each stage with what fields it uses and what it produces.
     *
     * We choose to put the $group at the end to prune our search space and because stages after
     * $group won't ever use block processing.
     */
    for (let i1 = 0; i1 < projectMatchStages.length; i1++) {
        const stage1 = projectMatchStages[i1];
        for (let i2 = 0; i2 < projectMatchStages.length; i2++) {
            const stage2 = projectMatchStages[i2];
            // Don't put the same stage twice in a row, it'll just be deduplicated in pipeline
            // optimization.
            if (i1 === i2) {
                continue;
            }
            for (const groupStage of groupStages) {
                // Any prior stage doesn't satify the requirements of a following stage, don't run
                // the query.
                if (!stageRequirementsMatch(stage1, stage2) ||
                    !stageRequirementsMatch(stage1, groupStage) ||
                    !stageRequirementsMatch(stage2, groupStage)) {
                    continue;
                }
                const pipeline = [stage1.stage, stage2.stage, groupStage.stage];
                compareClassicAndBP(pipeline, allowDiskUse);
            }
        }
    }
}

// Run with different combinations of allowDiskUse and force spilling.
runAggregations(false /*allowDiskUse*/, false /*forceSpilling*/);
runAggregations(true /*allowDiskUse*/, false /*forceSpilling*/);
runAggregations(true /*allowDiskUse*/, true /*forceSpilling*/);

MongoRunner.stopMongod(classicConn);
MongoRunner.stopMongod(bpConn);
