/**
 * Tests the idempotency of the _configsvrSetAllowMigrations command.
 *
 * TODO (SERVER-98118): Remove this test once 9.0 becomes last LTS.
 * _configsvrSetAllowMigrations belongs to the legacy (non-authoritative) protocol: it bumps the
 * collection placement version and relies on tellShardsToRefresh (i.e. the deprecated
 * _flushRoutingTableCacheUpdates(WithWriteConcern) commands) to propagate the change to shards.
 * Once shards are authoritative for collection metadata, that command is no longer served, so this
 * test no longer applies.
 */
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {skipTestIfAuthoritativeShardsEnabled} from "jstests/sharding/libs/sharding_util.js";

function runConfigsvrSetAllowMigrationsWithRetries(st, ns, lsid, txnNumber, allowMigrations) {
    let res;
    assert.soon(() => {
        res = st.configRS.getPrimary().adminCommand({
            _configsvrSetAllowMigrations: ns,
            allowMigrations: allowMigrations,
            collectionUUID: st.s.getCollection("config.collections").findOne({_id: ns}).uuid,
            lsid: lsid,
            txnNumber: txnNumber,
            writeConcern: {w: "majority"},
        });

        if (
            RetryableWritesUtil.isRetryableCode(res.code) ||
            RetryableWritesUtil.errmsgContainsRetryableCodeName(res.errmsg) ||
            (res.writeConcernError &&
                RetryableWritesUtil.isRetryableCode(res.writeConcernError.code))
        ) {
            return false; // Retry
        }

        return true;
    });

    return res;
}

const st = new ShardingTest({shards: 1});

skipTestIfAuthoritativeShardsEnabled(st.s, () => st.stop());

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

st.s.adminCommand({shardCollection: ns, key: {x: 1}});

let lsid = assert.commandWorked(st.s.getDB("admin").runCommand({startSession: 1})).id;

assert.eq(
    false,
    st.s.getCollection("config.collections").findOne({_id: ns}).hasOwnProperty("allowMigrations"),
);

assert.commandWorked(runConfigsvrSetAllowMigrationsWithRetries(st, ns, lsid, NumberLong(1), false));

let collectionMetadata = st.s.getCollection("config.collections").findOne({_id: ns});
assert.eq(true, collectionMetadata.hasOwnProperty("allowMigrations"));
assert.eq(false, collectionMetadata.allowMigrations);

// We should get a TransactionTooOld error if we try to re-execute the TXN with an older txnNumber
assert.commandFailedWithCode(
    runConfigsvrSetAllowMigrationsWithRetries(st, ns, lsid, NumberLong(0), true),
    ErrorCodes.TransactionTooOld,
);

// The command should be idempotent
assert.commandWorked(runConfigsvrSetAllowMigrationsWithRetries(st, ns, lsid, NumberLong(2), false));

collectionMetadata = st.s.getCollection("config.collections").findOne({_id: ns});
assert.eq(true, collectionMetadata.hasOwnProperty("allowMigrations"));
assert.eq(false, collectionMetadata.allowMigrations);

st.stop();
