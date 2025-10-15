/**
 * Tests that a standalone mongod and a mongos are able to pin code segments on startup when
 * 'lockCodeSegmentsInMemory=true'.
 * TODO (SERVER-75632): Re-enable this test on amazon linux once ulimits are configured.
 * @tags: [requires_increased_memlock_limits, incompatible_with_macos,
 * incompatible_with_windows_tls, incompatible_with_amazon_linux]
 */

const connD = MongoRunner.runMongod({setParameter: {lockCodeSegmentsInMemory: true}});
assert.neq(null, connD, "mongod was unable to start up");
assert.eq(0, MongoRunner.stopMongod(connD));

let configRS = new ReplSetTest({name: "configRS", nodes: 1});
configRS.startSet({configsvr: ""});
let replConfig = configRS.getReplSetConfig();
replConfig.configsvr = true;
configRS.initiate(replConfig);

const connS = MongoRunner.runMongos(
    {configdb: configRS.getURL(), setParameter: {lockCodeSegmentsInMemory: true}});
assert.neq(null, connS, "mongos was unable to start up");
assert.eq(0, MongoRunner.stopMongos(connS));

configRS.stopSet();
