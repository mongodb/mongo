/**
 * @tags: [
 *   requires_persistence,
 *   featureFlagEnableReplicasetTransitionToCSRS,
 * ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("replicaSetConfigShardMaintenanceMode startup flag incompatibility tests", function() {
    it("incompatible with standalone", () => {
        const error = assert.throws(() => {
            MongoRunner.runMongod({replicaSetConfigShardMaintenanceMode: ""});
        });
        assert.eq(error.name, "StopError");
        assert.eq(error.returnCode, 1);
    });

    it("incompatible with shardsvr flag", () => {
        const rs = new ReplSetTest({nodes: 1});

        const error = assert.throws(() => {
            rs.startSet({
                shardsvr: "",
                replicaSetConfigShardMaintenanceMode: "",
            });
        });
        assert.eq(error.name, "StopError");
        assert.eq(error.returnCode, 1);
    });
});

describe("replicaSetConfigShardMaintenanceMode startup flag compatibility tests", function() {
    beforeEach(() => {
        this.rs = new ReplSetTest({nodes: 1});
    });

    afterEach(() => {
        this.rs.initiate();
        this.rs.stopSet();
    });

    it("compatible with configsvr flag", () => {
        this.rs.startSet({
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });
    });

    it("compatible with replset flag", () => {
        this.rs.startSet({
            replSet: "replica_set_to_csrs_promotion",
            replicaSetConfigShardMaintenanceMode: "",
        });
    });
});
