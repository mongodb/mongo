/**
 * Tests that mongos can start up with --configOnly pointed at a plain (non-configsvr) replica set
 * and that this flag can be set via command line or config file.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {writeJSONConfigFile} from "jstests/libs/command_line/test_parsed_options.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("config-only mode startup", function () {
    before(function () {
        this.rs = new ReplSetTest({nodes: 1});
        this.rs.startSet();
        this.rs.initiate();

        // We insert a document into the replica set to mimic the cluster ID on the config server
        // since the mongoS requires this to complete startup. On a normal standby cluster, this
        // would already exist since the config server has full metadata.
        assert.commandWorked(
            this.rs.getPrimary().getDB("config").getCollection("version").insert({clusterId: ObjectId()}),
        );
    });

    after(function () {
        this.rs.stopSet();
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
