/**
 * Tests that mongos can start up with --configOnly pointed at a plain (non-configsvr) replica set
 * and that this flag can be set via command line or config file.
 *
 * @tags: [
 *   # The StandbyClusterTestFixture restarts nodes and expects persisted metadata to be preserved.
 *   requires_persistence,
 * ]
 */

import {StandbyClusterTestFixture} from "jstests/noPassthrough/libs/sharded_cluster_topology/standby_cluster_test_fixture.js";

const fixture = new StandbyClusterTestFixture({
    name: jsTestName(),
    shards: 1,
    rs: {nodes: 1},
    config: 3,
});
fixture.transitionToStandby();
const rs = fixture.standbyRS;

try {
    jsTest.log("Verifying mongos starts successfully with --configOnly against a plain replica set");
    const mongos = MongoRunner.runMongos({configdb: rs.getURL(), configOnly: true});
    assert.neq(null, mongos, "mongos was unable to start up");
    assert.commandWorked(mongos.adminCommand({hello: 1}));
    MongoRunner.stopMongos(mongos);

    jsTest.log(
        "Verifying mongos starts successfully with --configOnly against a plain replica set with config file");
    const configFilePath = MongoRunner.dataPath + "/config_only_mongos_config.json";
    writeFile(configFilePath, JSON.stringify({sharding: {configOnly: true}}));
    const mongosFromFile = MongoRunner.runMongos({configdb: rs.getURL(), config: configFilePath});
    assert.neq(null, mongosFromFile, "mongos was unable to start up");
    assert.commandWorked(mongosFromFile.adminCommand({hello: 1}));
    MongoRunner.stopMongos(mongosFromFile);

    jsTest.log("Verifying mongod rejects --configOnly");
    let threw = true;
    try {
        const mongod = MongoRunner.runMongod({configOnly: true});
        threw = false;
        MongoRunner.stopMongod(mongod);
    } catch (e) {
        assert.eq(e.returnCode, MongoRunner.EXIT_BADOPTIONS, "mongod exited with unexpected exit code");
    }
    assert.eq(threw, true, "mongod was able to start up with configOnly");
} finally {
    fixture.teardown();
}
