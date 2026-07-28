/**
 * Tests that the searchTaskExecutorMetrics section appears in serverStatus on both mongod and
 * mongos, and has the expected structure with diagnosticInfo, networkInterface, and connectionPool
 * sub-sections for both the mongot and searchIndex executors.
 *
 * @tags: [requires_sharding]
 */
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

if (_isWindows()) {
    quit();
}

// Validates the searchTaskExecutorMetrics section of serverStatus on the given connection.
// 'label' identifies the node in assertion messages.
function checkSearchTaskExecutorMetrics(conn, label) {
    jsTestLog(`Checking searchTaskExecutorMetrics on ${label}`);
    const status = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    const metrics = status.searchTaskExecutorMetrics;
    assert(metrics !== undefined, `${label}: searchTaskExecutorMetrics missing from serverStatus`);

    for (const executorName of ["mongot", "searchIndex"]) {
        const executor = metrics[executorName];
        assert(executor !== undefined,
               `${label}: ${executorName} missing from searchTaskExecutorMetrics`);

        for (const subSection of ["diagnosticInfo", "networkInterface", "connectionPool"]) {
            assert(
                executor.hasOwnProperty(subSection),
                `${label}: ${executorName} missing ${subSection} sub-section`,
            );
            assert(
                Object.keys(executor[subSection]).length > 0,
                `${label}: ${executorName}.${subSection} is empty`,
            );
        }
    }
}

// Standalone.
{
    const mongotmock = new MongotMock();
    mongotmock.start();
    const mongotHost = mongotmock.getConnection().host;

    const conn = MongoRunner.runMongod({
        setParameter: {
            mongotHost: mongotHost,
            searchIndexManagementHostAndPort: mongotHost,
        },
    });

    checkSearchTaskExecutorMetrics(conn, "standalone");

    MongoRunner.stopMongod(conn);
    mongotmock.stop();
}

// Sharded cluster.
{
    const mongotmock = new MongotMock();
    mongotmock.start();
    const mongotHost = mongotmock.getConnection().host;

    const setParameter = {
        mongotHost: mongotHost,
        searchIndexManagementHostAndPort: mongotHost,
    };

    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 2},
        other: {
            rsOptions: {setParameter: setParameter},
            mongosOptions: {setParameter: setParameter},
        },
    });

    checkSearchTaskExecutorMetrics(st.rs0.getPrimary(), "shard primary");
    checkSearchTaskExecutorMetrics(st.rs0.getSecondary(), "shard secondary");
    checkSearchTaskExecutorMetrics(st.s, "mongos");

    st.stop();
    mongotmock.stop();
}
