/**
 * @tags: [
 *   requires_persistence,
 *   featureFlagEnableReplicasetTransitionToCSRS,
 * ]
 */
import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
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

    it("incompatible with fcv changes", () => {
        const rs = new ReplSetTest({nodes: 1});
        rs.startSet({
            replSet: "replica_set_to_csrs_promotion",
            replicaSetConfigShardMaintenanceMode: "",
        });
        rs.initiate();

        assert.commandFailedWithCode(rs.getPrimary().adminCommand({
            setFeatureCompatibilityVersion: lastLTSFCV,
            confirm: true,
        }),
                                     ErrorCodes.IllegalOperation);

        rs.stopSet();
    });
});

describe("replicaSetConfigShardMaintenanceMode startup flag compatibility tests", function() {
    before(() => {
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

describe("restarting replicaset as configserver", function() {
    before(() => {
        this.rs = new ReplSetTest({nodes: 1});
    });

    beforeEach(() => {
        this.rs.startSet({replSet: "replica_set_to_csrs_promotion"});
        this.rs.initiate();
        this.stopSetOptions = {};
        this.rs.stopSet(null /* signal */, true /* forRestart */, this.stopSetOptions);
    });

    afterEach(() => {
        this.rs.stopSet(null, false, this.stopSetOptions);
    });

    it("restarting without replicaSetConfigShardMaintenanceMode flag fails", () => {
        TestData.cleanUpCoreDumpsFromExpectedCrash = true;
        try {
            assert.throws(() => {
                this.rs.startSet({
                    configsvr: "",
                    remember: false,
                },
                                 true);
            });
        } finally {
            TestData.cleanUpCoreDumpsFromExpectedCrash = false;
            this.stopSetOptions = {
                skipValidatingExitCode: true,
            };
        }
    });

    it("restarting with replicaSetConfigShardMaintenanceMode flag succeeds", () => {
        assert.doesNotThrow(() => {
            this.rs.startSet({
                configsvr: "",
                replicaSetConfigShardMaintenanceMode: "",
                remember: false,
            },
                             true);
        });
    });
});

describe("restarting a configserver as a replicaset", function() {
    before(() => {
        this.rs = new ReplSetTest({nodes: 1});
    });

    beforeEach(() => {
        this.rs.startSet({
            configsvr: "",
            remember: false,
        });
        const config = this.rs.getReplSetConfig();
        config.configsvr = true;
        this.rs.initiate(config);
        this.stopSetOptions = {};
        this.rs.stopSet(null /* signal */, true /* forRestart */, this.stopSetOptions);
    });

    afterEach(() => {
        this.rs.stopSet(null, false, this.stopSetOptions);
    });

    it("restarting without replicaSetConfigShardMaintenanceMode flag fails", () => {
        TestData.cleanUpCoreDumpsFromExpectedCrash = true;
        try {
            assert.throws(() => {
                this.rs.startSet({
                    replSet: "replica_set_to_csrs_promotion",
                    remember: false,
                },
                                 true);
            });
        } finally {
            TestData.cleanUpCoreDumpsFromExpectedCrash = false;
            this.stopSetOptions = {
                skipValidatingExitCode: true,
            };
        }
    });

    it("restarting with replicaSetConfigShardMaintenanceMode flag succeeds", () => {
        assert.doesNotThrow(() => {
            this.rs.startSet({
                replSet: "replica_set_to_csrs_promotion",
                replicaSetConfigShardMaintenanceMode: "",
                remember: false,
            },
                             true);
        });
    });
});

describe("transitions", function() {
    before(() => {
        this.doRollingRestart = (rs, startupFlags) => {
            for (const node of rs.getSecondaries()) {
                const id = this.rs.getNodeId(node);
                this.rs.stop(id, null, {}, {
                    forRestart: true,
                    waitPid: true,
                });
                assert.doesNotThrow(() => {
                    rs.start(id, {
                        ...startupFlags,
                        remember: false,
                    });
                });
            }
            const primaryId = rs.getNodeId(rs.getPrimary());
            rs.stepUp(rs.getSecondary());
            rs.stop(primaryId, null, {}, {
                forRestart: true,
                waitPid: true,
            });
            assert.doesNotThrow(() => {
                rs.start(primaryId, {
                    ...startupFlags,
                    remember: false,
                });
            });
        };
    });

    beforeEach(() => {
        this.rs = new ReplSetTest({nodes: 3});
    });

    afterEach(() => {
        this.rs.stopSet();
    });

    it("from replicaset to csrs", () => {
        this.rs.startSet({
            replSet: "replica_set_to_csrs_promotion",
            remember: false,
        });
        this.rs.initiate();
        this.doRollingRestart(this.rs, {
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });

        const config = this.rs.getReplSetConfigFromNode();
        config.configsvr = true;
        config.version = config.version + 1;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));
        this.doRollingRestart(this.rs, {
            configsvr: "",
        });
    });

    it("from csrs to replicaset", () => {
        this.rs.startSet({
            configsvr: "",
            remember: false,
        });
        const cfg = this.rs.getReplSetConfig();
        cfg.configsvr = true;
        this.rs.initiate(cfg);
        this.doRollingRestart(this.rs, {
            replSet: "replica_set_to_csrs_promotion",
            replicaSetConfigShardMaintenanceMode: "",
        });
        const config = this.rs.getReplSetConfigFromNode();
        assert.eq(config.configsvr, true);
        delete config.configsvr;
        config.version = config.version + 1;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));
        this.doRollingRestart(this.rs, {
            replSet: "replica_set_to_csrs_promotion",
        });
    });
});
