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
    const buildInfo = db.adminCommand("buildInfo");
    const isSanitizerEnabled = buildInfo.buildEnvironment.ccflags.includes('-fsanitize');
    const optimizationsEnabled = buildInfo.buildEnvironment.ccflags.includes('-O2');
    const debugBuild = buildInfo.debug;

    return debugBuild || !optimizationsEnabled || isSanitizerEnabled;
}

const classicDb = classicConn.getDB(jsTestName());
const bpDb = bpConn.getDB(jsTestName());
if (isSlowBuild(classicDb) || isSlowBuild(bpDb)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    MongoRunner.stopMongod(classicConn);
    MongoRunner.stopMongod(bpConn);
    quit();
}

const classicColl = classicDb.timeseries_group_aggregations;
const bpColl = bpDb.timeseries_group_aggregations;

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
let id = 0;
for (let metaIdx = 0; metaIdx < 5; metaIdx++) {
    // Mix of number and object meta values.
    const metaVal = metaIdx % 2 === 0 ? metaIdx : {metaA: metaIdx, metaB: metaIdx / 2};

    let currentDate = 0;
    for (let i = 0; i < 5; i++) {
        tsData.push({
            _id: id,
            t: new Date(datePrefix + currentDate - 100),
            m: metaVal,
            a: i,
            b: alphabet.charAt(i)
        });
        currentDate += 25;
        id++;
    }
}

assert.commandWorked(classicColl.insert(tsData));
assert.commandWorked(bpColl.insert(tsData));

function compareClassicAndBP(pipeline, allowDiskUse) {
    // End each pipeline with a sort by id so we can compare the results.
    pipeline = pipeline.concat([{$_internalInhibitOptimization: {}}, {$sort: {_id: 1}}]);

    const classicResults = classicColl.aggregate(pipeline, {allowDiskUse}).toArray();
    const bpResults = bpColl.aggregate(pipeline, {allowDiskUse}).toArray();

    function errFn() {
        jsTestLog(classicColl.explain().aggregate(pipeline, {allowDiskUse}));
        jsTestLog(bpColl.explain().aggregate(pipeline, {allowDiskUse}));

        return "Got different results for pipeline " + tojson(pipeline);
    }
    assert.eq(classicResults, bpResults, errFn);
}

// Cut down a path to just the top level.
function topLevelField(path) {
    if (path === null) {
        return null;
    }
    const pos = path.search(/[.]/);
    if (pos === -1) {
        return path;
    }
    return path.substr(0, pos);
}

// Create $project stages.
const projectStages = [
    {stage: {$project: {t: '$t'}}, uses: ['t'], produces: ['t']},
    {stage: {$project: {m: '$m'}}, uses: ['m'], produces: ['m']},

    // This is banned until SERVER-87961 is resolved.
    //{stage: {$project: {m: '$m.metaA'}}, uses: ['m'], produces: ['m']},
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

const addFieldsStages = [
    {stage: {$addFields: {t: new Date(datePrefix + 100)}}, uses: [], produces: allFields},
    {stage: {$addFields: {m: 5}}, uses: [], produces: allFields},
    {stage: {$addFields: {a: 2}}, uses: [], produces: allFields}
];

// Create $match stages.
const matchStages = [];
const matchComparisons = [
    {field: 't', comp: dateLowerBound},
    {field: 'm', comp: {metaA: 1, metaB: 1}},
    {field: 'm', comp: 3},
    {field: 'm.metaA', comp: 6},
    {field: 'a', comp: 2},
];
for (const matchComp of matchComparisons) {
    for (const comparator of ['$gt', '$lte', '$eq']) {
        matchStages.push({
            stage: {$match: {[matchComp.field]: {[comparator]: matchComp.comp}}},
            uses: [topLevelField(matchComp.field)],
            produces: allFields
        });
    }
}

// Create $group stages, which include one $count stage.
const groupStages = [
    // $count.
    {stage: {$count: 'c'}, uses: []},

    // $group with single key cases for each target accumulator.
    {stage: {$group: {_id: {m: '$m'}, gb: {$min: '$a'}}}, uses: ['m', 'a']},

    // $group with compound key cases since we don't want to try every combination.
    {stage: {$group: {_id: {t: '$t', m: '$m'}, gb: {$min: '$a'}}}, uses: ['t', 'm', 'a']},
    {stage: {$group: {_id: {a: '$a', m: '$m'}, gb: {$avg: '$t'}}}, uses: ['t', 'm', 'a']},
    {stage: {$group: {_id: {a: '$a', m: '$m.metaB'}, gb: {$avg: '$t'}}}, uses: ['t', 'm', 'a']},

    // $group with $dateTrunc.
    {
        stage: {$group: {_id: {$dateTrunc: {date: "$t", unit: "hour"}}, gb: {$max: '$a'}}},
        uses: ['t', 'a']
    },
];

for (const groupKey of [null, 't', 'm', 'm.metaA', 'a']) {
    const dollarGroupKey = groupKey === null ? null : '$' + groupKey;
    for (const accumulatorData of ['t', 'm', 'm.metaA', 'a']) {
        const dollarAccumulatorData = accumulatorData === null ? null : '$' + accumulatorData;
        // If the groupKey and the accumulated field is the same, the query is nonsensical.
        if (groupKey === accumulatorData) {
            continue;
        }
        for (const accumulator of ['$min', '$max', '$sum', '$avg']) {
            const uses = [topLevelField(accumulatorData)];
            // "null" is not a field we need from previous stages.
            if (groupKey !== null) {
                uses.push(topLevelField(groupKey));
            }
            groupStages.push({
                stage: {$group: {_id: dollarGroupKey, gb: {[accumulator]: dollarAccumulatorData}}},
                uses: uses
            });
        }
        // $top/$bottom accumulators have different syntax.
        for (const sortBy of ['t', 'm', 'a', 'm.metaA']) {
            if (sortBy === groupKey) {
                continue;
            }
            const uses = [topLevelField(accumulatorData), sortBy];
            if (groupKey !== null) {
                uses.push(topLevelField(groupKey));
            }
            for (const accumulator of ['$top', '$bottom']) {
                groupStages.push({
                    stage: {
                        $group: {
                            _id: dollarGroupKey,
                            gb: {
                                [accumulator]:
                                    {output: dollarAccumulatorData, sortBy: {[sortBy]: 1, _id: 1}}
                            }
                        }
                    },
                    uses: uses
                });
            }
            for (const accumulator of ['$topN', '$bottomN']) {
                groupStages.push({
                    stage: {
                        $group: {
                            _id: dollarGroupKey,
                            gb: {
                                [accumulator]: {
                                    n: 3,
                                    output: dollarAccumulatorData,
                                    sortBy: {[sortBy]: 1, _id: 1}
                                }
                            }
                        }
                    },
                    uses: uses
                });
            }
        }
    }
    const uses = groupKey === null ? [] : [topLevelField(groupKey)];
    groupStages.push({stage: {$group: {_id: dollarGroupKey, gb: {$count: {}}}}, uses: uses});
}

const projectMatchStages = [...projectStages, ...matchStages, ...addFieldsStages];

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
     * Try all combinations of $project and $match stages followed by a $group or $count at the end.
     * To avoid running unrealistic queries, we'll check what fields a stage needs to run. For
     * example a {$match: {m: 1}} needs the "m" field to do anything useful. If we have:
     *     [{$project: {a: 1}}, {$match: {m: 1}}]
     * the $project removes "m" so this is not a realistic query. For this reason we mark each stage
     * with what fields it uses and what it produces.
     *
     * We choose to put the $group or $count at the end to prune our search space and because stages
     * after these won't ever use block processing.
     */
    let numPipelinesRun = 0;
    for (let i1 = 0; i1 < projectMatchStages.length; i1++) {
        const stage1 = projectMatchStages[i1];
        for (let i2 = 0; i2 < projectMatchStages.length; i2++) {
            const stage2 = projectMatchStages[i2];

            // If these two stages don't match, skip running the query.
            if (!stageRequirementsMatch(stage1, stage2)) {
                continue;
            }

            // Don't put the same stage twice in a row, it'll just be deduplicated in pipeline
            // optimization.
            if (i1 === i2) {
                continue;
            }
            for (const groupStage of groupStages) {
                // Any prior stage doesn't satify the requirements of a following stage, don't run
                // the query.
                if (!stageRequirementsMatch(stage1, groupStage) ||
                    !stageRequirementsMatch(stage2, groupStage)) {
                    continue;
                }
                const pipeline = [stage1.stage, stage2.stage, groupStage.stage];
                compareClassicAndBP(pipeline, allowDiskUse);
                ++numPipelinesRun;
            }
        }
    }

    jsTestLog(`Ran ${numPipelinesRun} pipelines with allowDisk=${allowDiskUse},
                forceSpilling=${forceSpilling}`);
}

// Run with different combinations of allowDiskUse and force spilling.
runAggregations(false /*allowDiskUse*/, false /*forceSpilling*/);

MongoRunner.stopMongod(classicConn);
MongoRunner.stopMongod(bpConn);
