const mongod = MongoRunner.runMongod({
    sslMode: "disabled",
    setParameter: {oplogSamplingLogIntervalSeconds: 12, internalQueryCacheSize: 16}
});

try {
    // Check the presence of a warnings for deprecated sslMode parameter.
    assert.soon(() => checkLog.checkContainsWithCountJson(mongod, 23322, {}, 1, null, true),
                "Did not find expected deprecated warning for sslMode");
    // Check the presence of warnings for two deprecated server parameters.
    assert.soon(() => checkLog.checkContainsWithCountJson(mongod, 636300, {}, 2, null, true),
                "Did not find expected 2 deprecated warning log entries");
} finally {
    MongoRunner.stopMongod(mongod);
}
