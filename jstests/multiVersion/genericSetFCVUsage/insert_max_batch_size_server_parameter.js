
const dbpath = MongoRunner.dataPath + "insert_max_batch_size_server_parameter";
resetDbpath(dbpath);

const latest = "latest";
const default70BatchSize = 64;
const default80BatchSize = 500;

// Setup for functions for each server mode
function getOptions(binVersion, fcv, setParams) {
    let options = {'binVersion': binVersion};
    if (fcv) {
        options['setParameter'] = {"defaultStartupFCV": fcv};
    }
    if (setParams) {
        options['setParameter'] = options['setParameter'] || {};
        options['setParameter'] = {...options['setParameter'], ...setParams};
    }
    return options;
}

function startStandalone(fcvOptions) {
    const version = "last-lts";
    let options = {...fcvOptions, 'dbpath': dbpath};
    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, "mongod was unable to start up");
    const adminDB = conn.getDB("admin");
    return [adminDB, () => MongoRunner.stopMongod(conn)];
}

function startSharded(fcvOptions) {
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        config: 1,
        other: {configOptions: fcvOptions, shardOptions: fcvOptions, mongosOptions: fcvOptions}
    });

    const adminDB = st.rs0.getPrimary().getDB("admin");
    return [adminDB, () => st.stop()];
}

function startReplSet(fcvOptions) {
    const rst = new ReplSetTest({nodes: 3, nodeOptions: fcvOptions});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const adminDB = primary.getDB("admin");

    return [adminDB, () => rst.stopSet()];
}

// Test functions
function unsetTest(initFunc) {
    const version = "last-lts ";
    const options = getOptions(version);
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, lastLTSFCV);

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, default70BatchSize, tojson(batchSize));

    stopFunc();
}

function latestTest(initFunc) {
    // A 'latest' binary standalone should default to 'latestFCV'.
    const options = getOptions(latest, "8.0");
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, latestFCV);

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, default80BatchSize, tojson(batchSize));

    stopFunc();
}

function lastLtsTest(initFunc) {
    const options = getOptions(latest, "7.0");
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, lastLTSFCV);

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, default70BatchSize, tojson(batchSize));

    stopFunc();
}

function userOverrideOnStartupTest(initFunc) {
    const overrideBatchSize = 42;

    const options =
        getOptions(latest, "8.0", {"internalInsertMaxBatchSize": overrideBatchSize.toString()});
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, latestFCV);

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    stopFunc();
}

function userOverrideCmdTest(initFunc) {
    const options = getOptions(latest, "8.0");
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, latestFCV);

    const overrideBatchSize = 43;

    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, internalInsertMaxBatchSize: overrideBatchSize}));

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    stopFunc();
}

function upgradeTest(initFunc) {
    const options = getOptions(latest, "7.0");
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, lastLTSFCV);

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, default70BatchSize, tojson(batchSize));

    const upgradeFCV = "8.0";
    assert.commandWorked(adminDB.runCommand({
        setFeatureCompatibilityVersion: upgradeFCV,
        confirm: true,
        fromConfigServer: true,
    }));
    checkFCV(adminDB, upgradeFCV);
    const batchSizeAfterUpgrade =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(
        batchSizeAfterUpgrade.internalInsertMaxBatchSize, default80BatchSize, tojson(batchSize));

    stopFunc();
}

function downgradeTest(initFunc) {
    const options = getOptions(latest, "8.0");
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, latestFCV);

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, default80BatchSize, tojson(batchSize));

    const downgradeFCV = "7.0";
    assert.commandWorked(adminDB.runCommand({
        setFeatureCompatibilityVersion: downgradeFCV,
        confirm: true,
        fromConfigServer: true,
    }));
    checkFCV(adminDB, downgradeFCV);
    const batchSizeAfterDowngrade =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(
        batchSizeAfterDowngrade.internalInsertMaxBatchSize, default70BatchSize, tojson(batchSize));

    stopFunc();
}

function upgradeWithUserOverrideOnStartupTest(initFunc) {
    const overrideBatchSize = 42;
    const options =
        getOptions(latest, "7.0", {"internalInsertMaxBatchSize": overrideBatchSize.toString()});
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, lastLTSFCV);

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    const upgradeFCV = "8.0";
    assert.commandWorked(adminDB.runCommand({
        setFeatureCompatibilityVersion: upgradeFCV,
        confirm: true,
        fromConfigServer: true,
    }));
    checkFCV(adminDB, upgradeFCV);

    jsTestLog("Make sure that the parameter is still set to the value the user specified");
    const batchSizeAfterUpgrade =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(
        batchSizeAfterUpgrade.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    stopFunc();
}

function downgradeWithUserOverrideOnStartupTest(initFunc) {
    const overrideBatchSize = 42;
    const options =
        getOptions(latest, "8.0", {"internalInsertMaxBatchSize": overrideBatchSize.toString()});
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, latestFCV);

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    const downgradeFCV = "7.0";
    assert.commandWorked(adminDB.runCommand({
        setFeatureCompatibilityVersion: downgradeFCV,
        confirm: true,
        fromConfigServer: true,
    }));
    checkFCV(adminDB, downgradeFCV);
    jsTestLog("Make sure batch size is still set to the value the user specified");
    const batchSizeAfterDowngrade =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(
        batchSizeAfterDowngrade.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    stopFunc();
}

function upgradeWithUserOverrideWithCmdTest(initFunc) {
    const options = getOptions(latest, "7.0");
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, lastLTSFCV);

    const batchSizeBeforeOverride =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSizeBeforeOverride.internalInsertMaxBatchSize,
              default70BatchSize,
              tojson(batchSizeBeforeOverride));

    const overrideBatchSize = 43;
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, internalInsertMaxBatchSize: overrideBatchSize}));

    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    const upgradeFCV = "8.0";
    assert.commandWorked(adminDB.runCommand({
        setFeatureCompatibilityVersion: upgradeFCV,
        confirm: true,
        fromConfigServer: true,
    }));
    checkFCV(adminDB, upgradeFCV);
    jsTestLog("Make sure parameter is still set to the value the user set");
    const batchSizeAfterUpgrade =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(
        batchSizeAfterUpgrade.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    jsTestLog("Override again and make sure the new value takes effect");
    const secondOverrideBatchSize = 44;
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, internalInsertMaxBatchSize: secondOverrideBatchSize}));

    const batchSizeAfterOverride =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSizeAfterOverride.internalInsertMaxBatchSize,
              secondOverrideBatchSize,
              tojson(batchSizeAfterOverride));

    stopFunc();
}

function downgradeWithUserOverrideWithCmdTest(initFunc) {
    const options = getOptions(latest, "8.0");
    const [adminDB, stopFunc] = initFunc(options);
    checkFCV(adminDB, latestFCV);

    const batchSizeBeforeOverride =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSizeBeforeOverride.internalInsertMaxBatchSize,
              default80BatchSize,
              tojson(batchSizeBeforeOverride));

    const overrideBatchSize = 43;
    assert.commandWorked(
        adminDB.runCommand({setParameter: 1, internalInsertMaxBatchSize: overrideBatchSize}));
    const batchSize =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(batchSize.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    const downgradeFCV = "7.0";
    assert.commandWorked(adminDB.runCommand({
        setFeatureCompatibilityVersion: downgradeFCV,
        confirm: true,
        fromConfigServer: true,
    }));
    checkFCV(adminDB, downgradeFCV);
    jsTestLog("Make sure batch size is still the value that the user set");
    const batchSizeAfterDowngrade =
        assert.commandWorked(adminDB.runCommand({getParameter: 1, internalInsertMaxBatchSize: 1}));
    assert.eq(
        batchSizeAfterDowngrade.internalInsertMaxBatchSize, overrideBatchSize, tojson(batchSize));

    stopFunc();
}

// Run each test on each server mode
const tests = {
    "starting mongod with the last lts bin version should default to the parameter for the lower FCV":
        unsetTest,
    "starting mongod with the latest bin version should default to the parameter for the latestFCV ":
        latestTest,
    "starting mongod with FCV 7.0 should use the FCV 7.0 parameter value ": lastLtsTest,
    "user startup-time overrides for the parameter should override any defaults":
        userOverrideOnStartupTest,
    "user overrides for the parameter with setParameter command should override any defaults":
        userOverrideCmdTest,
    "upgrading the FCV should change the default value": upgradeTest,
    "downgrading the FCV should change the default value": downgradeTest,
    "upgrading the FCV should not override the value the user set on startup":
        upgradeWithUserOverrideOnStartupTest,
    "downgrading the FCV should not override the value the user set on startup":
        downgradeWithUserOverrideOnStartupTest,
    "upgrading the FCV should not override the value the user set with the setParameter command":
        upgradeWithUserOverrideWithCmdTest,
    "downgrading the FCV should not override the value the user set with the setParameter command":
        downgradeWithUserOverrideWithCmdTest,
};
for (const test in tests) {
    jsTestLog(test + " in standalone mode");
    tests[test](startStandalone);
    jsTestLog(test + " in sharded mode");
    tests[test](startSharded);
    jsTestLog(test + " in replSet mode");
    tests[test](startReplSet);
}
