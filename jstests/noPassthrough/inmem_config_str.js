// SERVER-28179 Test the startup of in-memory storage engine using --inMemoryEngineConfigString
if (jsTest.options().storageEngine !== "inMemory") {
    jsTestLog("Skipping test because storageEngine is not inMemory");
    quit();
}

var mongod = MongoRunner.runMongod({
    storageEngine: 'inMemory',
    inMemoryEngineConfigString: 'eviction=(threads_min=1)',
});
assert.neq(null, mongod, "mongod failed to started up with --inMemoryEngineConfigString");

MongoRunner.stopMongod(mongod);