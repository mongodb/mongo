/**
 * @tags: [
 *   requires_persistence,
 *   requires_fcv_83,
 * ]
 */
import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("replicaSetConfigShardMaintenanceMode startup flag incompatibility tests", function () {
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

        assert.commandFailedWithCode(
            rs.getPrimary().adminCommand({
                setFeatureCompatibilityVersion: lastLTSFCV,
                confirm: true,
            }),
            ErrorCodes.IllegalOperation,
        );

        rs.stopSet();
    });
});

describe("replicaSetConfigShardMaintenanceMode startup flag compatibility tests", function () {
    before(() => {
        this.rs = new ReplSetTest({nodes: 1});
    });

    afterEach(() => {
        this.rs.initiate();
        assert.soon(() => {
            return checkLog.getLogMessage(this.rs.getPrimary(), "10718700") != null;
        });
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

describe("restarting replicaset as configserver", function () {
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
                this.rs.startSet(
                    {
                        configsvr: "",
                        remember: false,
                    },
                    true,
                );
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
            this.rs.startSet(
                {
                    configsvr: "",
                    replicaSetConfigShardMaintenanceMode: "",
                    remember: false,
                },
                true,
            );
        });
    });
});

describe("restarting a configserver as a replicaset", function () {
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
                this.rs.startSet(
                    {
                        replSet: "replica_set_to_csrs_promotion",
                        remember: false,
                    },
                    true,
                );
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
            this.rs.startSet(
                {
                    replSet: "replica_set_to_csrs_promotion",
                    replicaSetConfigShardMaintenanceMode: "",
                    remember: false,
                },
                true,
            );
        });
    });
});

describe("transitions", function () {
    before(() => {
        this.doRollingRestart = (rs, startupFlags) => {
            rs.awaitReplication();
            for (const node of rs.getSecondaries()) {
                const id = rs.getNodeId(node);
                rs.stop(
                    id,
                    null,
                    {},
                    {
                        forRestart: true,
                        waitPid: true,
                    },
                );
                assert.doesNotThrow(() => {
                    rs.start(id, {
                        ...startupFlags,
                        remember: false,
                    });
                });
            }
            const primaryId = rs.getNodeId(rs.getPrimary());
            rs.stepUp(rs.getSecondary());
            rs.stop(
                primaryId,
                null,
                {},
                {
                    forRestart: true,
                    waitPid: true,
                },
            );
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

    it("multiple state changes", () => {
        this.rs.startSet({
            replSet: "replica_set_to_csrs_promotion",
            remember: false,
        });
        this.rs.initiate();
        this.doRollingRestart(this.rs, {
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });

        let config = this.rs.getReplSetConfigFromNode();
        config.configsvr = true;
        config.version = config.version + 1;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));
        this.doRollingRestart(this.rs, {
            configsvr: "",
        });

        this.doRollingRestart(this.rs, {
            replSet: "replica_set_to_csrs_promotion",
            replicaSetConfigShardMaintenanceMode: "",
        });

        config = this.rs.getReplSetConfigFromNode();
        assert.eq(config.configsvr, true);
        delete config.configsvr;
        config.version = config.version + 1;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));
        this.doRollingRestart(this.rs, {
            replSet: "replica_set_to_csrs_promotion",
        });
    });

    it("multiple state changes with full promotion", () => {
        this.rs.startSet({
            replSet: "replica_set_to_csrs_promotion",
            remember: false,
        });
        this.rs.initiate();
        this.doRollingRestart(this.rs, {
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });

        let config = this.rs.getReplSetConfigFromNode();
        config.configsvr = true;
        config.version = config.version + 1;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));
        this.doRollingRestart(this.rs, {
            configsvr: "",
        });

        let mongos = MongoRunner.runMongos({configdb: this.rs.getURL()});
        assert.commandWorked(mongos.getDB("admin").runCommand({"transitionFromDedicatedConfigServer": 1}));

        this.doRollingRestart(this.rs, {
            replSet: "replica_set_to_csrs_promotion",
            replicaSetConfigShardMaintenanceMode: "",
        });

        config = this.rs.getReplSetConfigFromNode();
        assert.eq(config.configsvr, true);
        delete config.configsvr;
        config.version = config.version + 1;
        assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));
        this.doRollingRestart(this.rs, {
            replSet: "replica_set_to_csrs_promotion",
        });

        MongoRunner.stopMongos(mongos);
    });
});

describe("operations during rolling restart", function () {
    before(() => {
        this.restartASecondaryAndStepItUp = (rs, startupFlags) => {
            rs.awaitReplication();
            const secondary = rs.getSecondary();
            const id = rs.getNodeId(secondary);
            rs.stop(
                id,
                null,
                {},
                {
                    forRestart: true,
                    waitPid: true,
                },
            );
            assert.doesNotThrow(() => {
                rs.start(id, {
                    ...startupFlags,
                    remember: false,
                });
            });
            rs.stepUp(secondary);
        };

        this.restartAllSecondaries = (rs, startupFlags) => {
            rs.awaitReplication();
            for (const secondary of rs.getSecondaries()) {
                const id = rs.getNodeId(secondary);
                rs.stop(
                    id,
                    null,
                    {},
                    {
                        forRestart: true,
                        waitPid: true,
                    },
                );
                assert.doesNotThrow(() => {
                    rs.start(id, {
                        ...startupFlags,
                        remember: false,
                    });
                });
            }
            rs.awaitReplication();
        };
    });

    beforeEach(() => {
        this.rs = new ReplSetTest({nodes: 3});
    });

    afterEach(() => {
        this.rs.stopSet();
    });

    it("read during transition from replicaset to csrs", () => {
        this.rs.startSet({
            replSet: "replica_set_to_csrs_promotion",
            remember: false,
        });
        this.rs.initiate();

        assert.commandWorked(this.rs.getPrimary().getDB("foo").bar.insertOne({a: 42}));

        this.restartAllSecondaries(this.rs, {
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });

        assert.eq(this.rs.getPrimary().getDB("foo").bar.count({}), 1);
        this.rs.awaitSecondaryNodes();
        assert.eq(this.rs.getSecondary().getDB("foo").bar.count({}), 1);
    });

    it("read during transition from csrs to replicaset", () => {
        this.rs.startSet({
            configsvr: "",
            remember: false,
        });
        const cfg = this.rs.getReplSetConfig();
        cfg.configsvr = true;
        this.rs.initiate(cfg);

        assert.commandWorked(this.rs.getPrimary().getDB("foo").bar.insertOne({a: 42}));

        this.restartASecondaryAndStepItUp(this.rs, {
            replSet: "replica_set_to_csrs_promotion",
            replicaSetConfigShardMaintenanceMode: "",
        });

        assert.eq(this.rs.getPrimary().getDB("foo").bar.count({}), 1);
        this.rs.awaitSecondaryNodes();
        assert.eq(this.rs.getSecondary().getDB("foo").bar.count({}), 1);
    });

    it("write-read during transition from replicaset to csrs", () => {
        this.rs.startSet({
            replSet: "replica_set_to_csrs_promotion",
            remember: false,
        });
        this.rs.initiate();

        this.restartASecondaryAndStepItUp(this.rs, {
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });

        assert.commandWorked(
            this.rs
                .getPrimary()
                .getDB("foo")
                .bar.insertOne({a: 42}, {writeConcern: {w: 3}}),
        );

        assert.eq(this.rs.getPrimary().getDB("foo").bar.count({}), 1);
        this.rs.awaitSecondaryNodes();
        assert.eq(this.rs.getSecondary().getDB("foo").bar.count({}), 1);
    });

    it("write-read during transition from csrs to replicaset", () => {
        this.rs.startSet({
            configsvr: "",
            remember: false,
        });
        const cfg = this.rs.getReplSetConfig();
        cfg.configsvr = true;
        this.rs.initiate(cfg);

        this.restartASecondaryAndStepItUp(this.rs, {
            replSet: "replica_set_to_csrs_promotion",
            replicaSetConfigShardMaintenanceMode: "",
        });

        assert.commandWorked(
            this.rs
                .getPrimary()
                .getDB("foo")
                .bar.insertOne({a: 42}, {writeConcern: {w: 3}}),
        );

        assert.eq(this.rs.getPrimary().getDB("foo").bar.count({}), 1);
        this.rs.awaitSecondaryNodes();
        assert.eq(this.rs.getSecondary().getDB("foo").bar.count({}), 1);
    });
});
