/**
 * Tests that the persisted samples collection is dropped on every node when the FCV is downgraded
 * to a version where featureFlagPersistentStats is disabled, and that no other collection is.
 *
 * @tags: [featureFlagPersistentStats, future_git_tag_incompatible, uses_parallel_shell]
 **/
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {
    assertNoSamplesColl,
    assertSamplesCollClustered,
} from "jstests/libs/query/persistent_samples_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = "coll";

// Two databases, so the drop has to cover every database holding samples rather than just one.
const dbNames = [jsTestName() + "_a", jsTestName() + "_b"];

// Collections the downgrade must leave alone: the analyzed collection, a plain one, one whose name
// contains the samples name, and the sibling persistent-stats collection.
const keptCollNames = [collName, "keep", "samples", "system.stats.field_stats"];

function setFCV(db, version) {
    jsTest.log.info("Setting feature compatibility version", {version});
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: version, confirm: true}));
}

function persistSample(db) {
    assert.commandWorked(db[collName].insert([{a: 1}, {a: 2}, {a: 2}, {a: 3}]));
    assert.commandWorked(
        db.runCommand({analyze: collName, mode: "sample", samplingMethod: "random"}),
    );
}

function createKeptColls(db) {
    for (const name of keptCollNames) {
        assert.commandWorked(db[name].insertOne({}));
    }
}

function assertKeptCollsPresent(db) {
    const collNames = db.getCollectionNames();
    for (const name of keptCollNames) {
        assert(collNames.includes(name), "expected the collection to survive the downgrade", {
            name,
            dbName: db.getName(),
            collNames,
        });
    }
}

// Runs 'check' against every database in 'dbNames' on every non-config node, so the presence and
// the absence of the samples collection are asserted over the same nodes. The samples are
// shard-local, but mongos proxies the primary shard's listCollections.
function forEachNodeAndDb(conn, check) {
    for (const host of DiscoverTopology.findNonConfigNodes(conn)) {
        const node = new Mongo(host);
        try {
            node.setSecondaryOk();
            for (const dbName of dbNames) {
                check(node.getDB(dbName));
            }
        } finally {
            node.close();
        }
    }
}

const deployments = [
    {
        name: "replica set",
        setUp: () => {
            const rst = new ReplSetTest({nodes: 2});
            rst.startSet();
            rst.initiate();
            const primary = rst.getPrimary();
            return {fixture: rst, adminConn: primary, statsRst: rst};
        },
        tearDown: (fixture) => fixture.stopSet(),
    },
    {
        name: "sharded cluster",
        setUp: () => {
            const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
            // analyze goes through mongos, which routes it to the shard owning the collection.
            // The samples themselves are shard-local, so the assertions below still look at the
            // shard rather than at mongos.
            return {fixture: st, adminConn: st.s, statsRst: st.rs0};
        },
        tearDown: (fixture) => fixture.stop(),
    },
];

for (const deployment of deployments) {
    for (const downgradeFCV of [lastLTSFCV, lastContinuousFCV]) {
        describe(`Persistent stats on ${deployment.name} downgrading to FCV ${downgradeFCV}`, function () {
            before(function () {
                jsTest.log.info("Starting deployment", {deployment: deployment.name});
                const {fixture, adminConn, statsRst} = deployment.setUp();
                this.fixture = fixture;
                this.adminConn = adminConn;
                this.adminDB = adminConn.getDB("admin");
                this.dbs = dbNames.map((dbName) => adminConn.getDB(dbName));
                this.statsRst = statsRst;

                jsTest.log.info("Persisting a sample at the latest FCV", {dbNames});
                for (const db of this.dbs) {
                    persistSample(db);
                    createKeptColls(db);
                }
                this.statsRst.awaitReplication();
                forEachNodeAndDb(this.adminConn, assertSamplesCollClustered);
                forEachNodeAndDb(this.adminConn, assertKeptCollsPresent);
            });

            after(function () {
                jsTest.log.info("Tearing down deployment", {deployment: deployment.name});
                deployment.tearDown(this.fixture);
            });

            describe("When downgrading", function () {
                before(function () {
                    setFCV(this.adminDB, downgradeFCV);
                    this.statsRst.awaitReplication();
                });

                it("Should drop the samples collection on every node", function () {
                    forEachNodeAndDb(this.adminConn, assertNoSamplesColl);
                });

                it("Should keep the other collections on every node", function () {
                    forEachNodeAndDb(this.adminConn, assertKeptCollsPresent);
                });
            });
        });
    }
}

// A sample can be generated before the FCV changes and persisted after it, because analyze reads
// the flag before its collection scan and writes after it. These cases park each side with a
// failpoint so both orderings can be checked deterministically. They run on the replica set only,
// since the interleaving is per node.
const raceDeployment = deployments[0];

describe(`Sample persisted while downgrading on ${raceDeployment.name}`, function () {
    before(function () {
        jsTest.log.info("Starting deployment", {deployment: raceDeployment.name});
        const {fixture, adminConn, statsRst} = raceDeployment.setUp();
        this.fixture = fixture;
        this.adminConn = adminConn;
        this.adminDB = adminConn.getDB("admin");
        this.dbs = dbNames.map((dbName) => adminConn.getDB(dbName));
        this.statsRst = statsRst;

        for (const db of this.dbs) {
            persistSample(db);
        }
        this.statsRst.awaitReplication();
        forEachNodeAndDb(this.adminConn, assertSamplesCollClustered);
    });

    after(function () {
        jsTest.log.info("Tearing down deployment", {deployment: raceDeployment.name});
        raceDeployment.tearDown(this.fixture);
    });

    it("Should not finish the downgrade before the samples are dropped", function () {
        const failPoint = configureFailPoint(this.adminConn, "hangBeforeDroppingPersistedSamples");
        const awaitSetFCV = startParallelShell(
            funWithArgs(function (version) {
                assert.commandWorked(
                    db.adminCommand({setFeatureCompatibilityVersion: version, confirm: true}),
                );
            }, lastLTSFCV),
            this.adminConn.port,
        );

        failPoint.wait();
        // The downgrade is parked before the drop, so the samples must still be there.
        forEachNodeAndDb(this.adminConn, assertSamplesCollClustered);

        failPoint.off();
        awaitSetFCV();
        this.statsRst.awaitReplication();

        forEachNodeAndDb(this.adminConn, assertNoSamplesColl);
    });

    describe("When the sample is persisted during the downgrade", function () {
        before(function () {
            // The case above left the cluster downgraded.
            setFCV(this.adminDB, latestFCV);
            this.statsRst.awaitReplication();
            for (const db of this.dbs) {
                persistSample(db);
            }
            this.statsRst.awaitReplication();
            forEachNodeAndDb(this.adminConn, assertSamplesCollClustered);
        });

        it("Should reject it instead of recreating the collection", function () {
            const failPoint = configureFailPoint(this.adminConn, "hangBeforePersistingSample");
            const awaitAnalyze = startParallelShell(
                funWithArgs(
                    function (dbName, collName) {
                        const res = db.getSiblingDB(dbName).runCommand({
                            analyze: collName,
                            mode: "sample",
                            samplingMethod: "random",
                        });
                        assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);
                    },
                    dbNames[0],
                    collName,
                ),
                this.adminConn.port,
            );

            failPoint.wait();
            setFCV(this.adminDB, lastLTSFCV);
            this.statsRst.awaitReplication();
            failPoint.off();
            awaitAnalyze();

            forEachNodeAndDb(this.adminConn, assertNoSamplesColl);
        });
    });
});
