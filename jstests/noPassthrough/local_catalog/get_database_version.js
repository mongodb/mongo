/*
 * Test getDatabaseVersion command after the effects of database DDLs.
 *
 * @tags: [
 *     featureFlagShardAuthoritativeDbMetadataCRUD,
 *     featureFlagShardAuthoritativeDbMetadataDDL,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getDatabaseVersionResponse(conn, dbName) {
    const responseFields = ["isPrimaryShardForDb", "dbVersion"];
    const dbVersionFields = ["uuid", "timestamp", "lastMod"];

    const response = conn.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(response);

    responseFields.forEach((field) =>
        assert(response[field], `Missing or null field ${field} in getDatabaseVersion response ${tojson(response)}`),
    );
    dbVersionFields.forEach((field) =>
        assert(
            response.dbVersion[field],
            `Missing  or null nested field ${field} in dbVersion response field ${tojson(response)}`,
        ),
    );
    return response;
}

const st = new ShardingTest({shards: 2});
const dbName = "testDb";
const db = st.s.getDB(dbName);

assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const uponDatabaseCreated = getDatabaseVersionResponse(st.shard0, dbName);
assert(uponDatabaseCreated.isPrimaryShardForDb);

// Pause movePrimary after entering the critical section.
const movePrimaryHang = configureFailPoint(st.rs0.getPrimary(), "hangAfterMovePrimaryCriticalSection");
const awaitResult = startParallelShell(
    funWithArgs(
        function (dbName, shard) {
            assert.commandWorked(db.adminCommand({movePrimary: dbName, to: shard}));
        },
        dbName,
        st.shard1.shardName,
    ),
    st.s.port,
);

movePrimaryHang.wait();

// Check that during movePrimary, the dbVersion can be consulted but we do not care about the
// result, as it is unstable.
assert.commandWorked(st.shard0.adminCommand({getDatabaseVersion: dbName}));

// Allow the movePrimary operation to finish on the source cluster.
movePrimaryHang.off();
awaitResult();

const uponPrimaryMoved = getDatabaseVersionResponse(st.shard1, dbName);
assert(uponPrimaryMoved.isPrimaryShardForDb);

st.stop();
