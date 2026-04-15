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
import {writeJSONConfigFile} from "jstests/libs/command_line/test_parsed_options.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("config-only mode startup", function () {
    before(function () {
        this.fixture = new StandbyClusterTestFixture({
            name: "config_only_mode_startup",
            shards: 1,
            rs: {nodes: 1},
            config: {nodes: 1},
        });
        this.fixture.transitionToStandby();
        this.rs = this.fixture.standbyRS;
    });

    after(function () {
        this.fixture.teardown();
    });

    it("Command line options", function () {
        jsTest.log.info("Verifying mongos starts successfully with --configOnly against a plain replica set");
        const mongos = MongoRunner.runMongos({configdb: this.rs.getURL(), configOnly: true});

        assert.neq(null, mongos, "mongos was unable to start up");
        assert.commandWorked(mongos.adminCommand({hello: 1}));

        MongoRunner.stopMongos(mongos);
    });

    it("Config file options", function () {
        jsTest.log.info(
            "Verifying mongos starts successfully with --configOnly against a plain replica set with config file",
        );
        const configFile = writeJSONConfigFile("config_only_mongos_config", {
            sharding: {configOnly: true},
        });
        const mongos = MongoRunner.runMongos({configdb: this.rs.getURL(), config: configFile});

        assert.neq(null, mongos, "mongos was unable to start up");
        assert.commandWorked(mongos.adminCommand({hello: 1}));

        MongoRunner.stopMongos(mongos);
    });

    it("Option not present on mongoD", function () {
        let threw = true;
        try {
            const mongod = MongoRunner.runMongod({configOnly: true});
            threw = false;
            MongoRunner.stopMongod(mongod);
        } catch (e) {
            assert.eq(e.returnCode, MongoRunner.EXIT_BADOPTIONS, "mongod exited with unexpected exit code");
        }
        assert.eq(threw, true, "mongod was able to start up with configOnly");
    });
});
