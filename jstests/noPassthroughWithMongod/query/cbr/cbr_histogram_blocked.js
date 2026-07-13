// @tags: [
//   requires_fcv_90,
//   disables_test_commands,
// ]

TestData.enableTestCommands = false;
const mongo = MongoRunner.runMongod();

assert.commandFailedWithCode(
    mongo.getDB("admin").adminCommand({
        setParameter: 1,
        internalQueryCBRCEMode: "histogramCE",
    }),
    ErrorCodes.BadValue,
);

assert.commandFailedWithCode(
    mongo.getDB("admin").adminCommand({
        setParameter: 1,
        automaticCEPlanRankingStrategy: "HistogramCEWithHeuristicFallback",
    }),
    ErrorCodes.BadValue,
);

assert.commandWorked(
    mongo.getDB("admin").adminCommand({
        setParameter: 1,
        internalQueryPlanRanker: "mixed",
    }),
);

assert.commandWorked(
    mongo.getDB("admin").adminCommand({
        setParameter: 1,
        automaticCEPlanRankingStrategy: "CBRForNoMultiplanningResults",
    }),
);

MongoRunner.stopMongod(mongo);
