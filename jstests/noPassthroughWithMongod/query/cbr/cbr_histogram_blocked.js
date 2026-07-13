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

assert.commandWorked(
    mongo.getDB("admin").adminCommand({
        setParameter: 1,
        internalQueryPlanRanker: "mixed",
        internalQueryCBRCEMode: "samplingCE",
    }),
);

assert.commandWorked(
    mongo.getDB("admin").adminCommand({
        setParameter: 1,
        internalQueryMixedPlanRankingStrategy: "NoMultiplanningResults",
    }),
);

MongoRunner.stopMongod(mongo);
