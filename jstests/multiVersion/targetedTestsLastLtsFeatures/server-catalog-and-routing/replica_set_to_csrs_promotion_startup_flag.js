/**
 * @tags: [
 *   featureFlagEnableReplicasetTransitionToCSRS,
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("replicaSetConfigShardMaintenanceMode startup flag incompatibility tests", function() {
    it("incompatible with disabled featureFlagEnableReplicasetTransitionToCSRS", () => {
        const rs = new ReplSetTest({
            nodes: [{
                setParameter: {defaultStartupFCV: lastLTSFCV},
            }]
        });
        rs.startSet({
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });

        TestData.cleanUpCoreDumpsFromExpectedCrash = true;
        assert.throws(function() {
            rs.initiate();
        });
        const SIGTERM = 15;
        rs.stopSet(SIGTERM, false, {
            skipValidatingExitCode: true,
        });
    });
});
