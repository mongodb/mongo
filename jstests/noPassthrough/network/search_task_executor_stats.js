/**
 * Tests that the searchTaskExecutorMetrics section appears in serverStatus on both mongod and
 * mongos, and has the expected structure with diagnosticInfo, networkInterface, and connectionPool
 * sub-sections for both the mongot and searchIndex executors.
 *
 * @tags: [requires_sharding]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (_isWindows()) {
    quit();
}

// Shared behavior: registers describe/it blocks that validate searchTaskExecutorMetrics.
// Callers must set this.conn before these tests run (e.g. in a before/beforeEach hook).
function searchTaskExecutorMetricsTests() {
    beforeEach(function () {
        const status = assert.commandWorked(this.conn.adminCommand({serverStatus: 1}));
        this.metrics = status.searchTaskExecutorMetrics;
    });

    it("has searchTaskExecutorMetrics section", function () {
        assert(this.metrics !== undefined, "searchTaskExecutorMetrics missing from serverStatus");
    });

    for (const executorName of ["mongot", "searchIndex"]) {
        describe(executorName, function () {
            it("has diagnosticInfo sub-section", function () {
                const executor = this.metrics[executorName];
                assert(executor.hasOwnProperty("diagnosticInfo"), `${executorName} missing diagnosticInfo sub-section`);
                assert(Object.keys(executor.diagnosticInfo).length > 0, `${executorName}.diagnosticInfo is empty`);
            });

            it("has networkInterface sub-section", function () {
                const executor = this.metrics[executorName];
                assert(
                    executor.hasOwnProperty("networkInterface"),
                    `${executorName} missing networkInterface sub-section`,
                );
                assert(Object.keys(executor.networkInterface).length > 0, `${executorName}.networkInterface is empty`);
            });

            it("has connectionPool sub-section", function () {
                const executor = this.metrics[executorName];
                assert(executor.hasOwnProperty("connectionPool"), `${executorName} missing connectionPool sub-section`);
                assert(Object.keys(executor.connectionPool).length > 0, `${executorName}.connectionPool is empty`);
            });
        });
    }
}

describe("searchTaskExecutorMetrics in serverStatus", function () {
    describe("standalone", function () {
        before(function () {
            this.mongotmock = new MongotMock();
            this.mongotmock.start();
            const mongotHost = this.mongotmock.getConnection().host;
            this.conn = MongoRunner.runMongod({
                setParameter: {
                    mongotHost: mongotHost,
                    searchIndexManagementHostAndPort: mongotHost,
                },
            });
        });

        after(function () {
            MongoRunner.stopMongod(this.conn);
            this.mongotmock.stop();
        });

        searchTaskExecutorMetricsTests();
    });

    describe("sharded", function () {
        before(function () {
            this.mongotmock = new MongotMock();
            this.mongotmock.start();
            const mongotHost = this.mongotmock.getConnection().host;
            this.st = new ShardingTest({
                shards: 1,
                rs: {nodes: 2},
                other: {
                    rsOptions: {
                        setParameter: {
                            mongotHost: mongotHost,
                            searchIndexManagementHostAndPort: mongotHost,
                        },
                    },
                    mongosOptions: {
                        setParameter: {
                            mongotHost: mongotHost,
                            searchIndexManagementHostAndPort: mongotHost,
                        },
                    },
                },
            });
        });

        after(function () {
            this.st.stop();
            this.mongotmock.stop();
        });

        describe("shard primary", function () {
            before(function () {
                this.conn = this.st.rs0.getPrimary();
            });
            searchTaskExecutorMetricsTests();
        });

        describe("shard secondary", function () {
            before(function () {
                this.conn = this.st.rs0.getSecondary();
            });
            searchTaskExecutorMetricsTests();
        });

        describe("mongos", function () {
            before(function () {
                this.conn = this.st.s;
            });
            searchTaskExecutorMetricsTests();
        });
    });
});
