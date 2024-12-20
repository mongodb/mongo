/*
 * Verifies the explicit creation of system.sessions always end up being correct without failing.
 * The request is silently overridden as a sharded creation with shardKey {_id:1}
 *
 * @tags: [
 *    requires_fcv_81,
 *    config_shard_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const kConfig = "config";
const kSystemSessions = "system.sessions";
const kNs = kConfig + "." + kSystemSessions;
const kExpectedShardKey = {
    _id: 1
};
const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    other: {
        mongosOptions: {
            setParameter: {
                // Make refresh behave as if disabled to prevent automatic system.sessions creation
                disableLogicalSessionCacheRefresh: true,
            }
        },
        configOptions: {
            setParameter: {
                // Make refresh behave as if disabled to prevent automatic system.sessions creation
                disableLogicalSessionCacheRefresh: true,
            }
        },
        rsOptions: {
            setParameter: {
                // Make refresh behave as if disabled to prevent automatic system.sessions creation
                disableLogicalSessionCacheRefresh: true,
            }
        }
    }
});

function cleanUpSystemSessions(st) {
    // Unshard and drop system.sessions.
    st.rs0.getPrimary().getDB(kConfig).getCollection(kSystemSessions).drop();
    let uuid = st.config.collections.find({_id: kNs}).toArray()[0].uuid;
    st.config.collections.remove({_id: kNs});
    st.config.chunks.remove({uuid: uuid});
    assert.eq(0, st.config.collections.find().toArray().length);
    assert.eq(0, st.config.chunks.find().toArray().length);
    // force a refresh
    st.s.getDB(kConfig).getCollection(kSystemSessions).findOne();
}

function runTest(st, createFn) {
    cleanUpSystemSessions(st);

    let createRes = createFn(st.s.getDB(kConfig));
    if (!createRes.ok) {
        assert.commandFailedWithCode(createRes, [ErrorCodes.Unauthorized]);
    }

    // Make sure system.session exists as sharded
    let result = st.config.collections.find({_id: kNs}).toArray();
    assert.eq(1,
              result.length,
              "config.system.collection must exists as sharded, but found " + tojson(result));
    assert.eq(kExpectedShardKey,
              result[0].key,
              "config.system.collection found with incorrect shard key " + tojson(result));
    assert.neq(true,
               result[0].unsplittable,
               "config.system.collection must exists as sharded, but found " + tojson(result));
}

jsTest.log("Creating system.sessions as unsharded");
{
    runTest(st, (db) => {
        return db.createCollection(kSystemSessions);
    });
}

jsTest.log("Creating system.sessions as unsharded with params");
{
    runTest(st, (db) => {
        return db.createCollection(kSystemSessions, {capped: true, size: 5000});
    });
}

jsTest.log("Creating system.sessions as sharded with wrong shard key");
{
    runTest(st, (db) => {
        return db.adminCommand({shardCollection: kNs, key: {x: 1}});
    });
}

jsTest.log("Shard system.sessions as timeseries");
{
    runTest(st, (db) => {
        return db.adminCommand({
            shardCollection: kNs,
            key: {meta: 1},
            timeseries: {timeField: "time", metaField: "meta"}
        });
    });
}

jsTest.log("Shard system.sessions with {_id:1}");
{
    runTest(st, (db) => {
        return db.adminCommand({shardCollection: kNs, key: {_id: 1}});
    });
}

st.stop();
