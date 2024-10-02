/* Test that both transaction and non-transaction snapshot reads on pre-image and change collections
 * are not allowed.
 *
 */

import {getPreImagesCollection} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    ChangeStreamMultitenantReplicaSetTest
} from "jstests/serverless/libs/change_collection_util.js";

// Shard server role is needed to run commands against the config database in a transaction.
const st = new ShardingTest({shards: {rs0: {nodes: 1}}});
const db = st.rs0.getPrimary().getDB('test');

const replicaSet = new ChangeStreamMultitenantReplicaSetTest({
    nodes: 1,
    name: "replicaSetTest",
    nodeOptions: {shardsvr: ""},
});
assert.commandWorked(st.s.adminCommand({addShard: replicaSet.getURL(), name: "replicaSetTest"}));

const primary = replicaSet.getPrimary();
const tenantId = ObjectId();

const tenantConn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(primary.host, tenantId);
assert.commandWorked(
    tenantConn.getDB("admin").runCommand({setChangeStreamState: 1, enabled: true}));

const preImagesCollection = getPreImagesCollection(db.getMongo());
const changeCollection = tenantConn.getDB("config").system.change_collection;

const collAndErrors = [
    {coll: preImagesCollection, error: 7829600, conn: db},
    {coll: changeCollection, error: 7829601, conn: tenantConn.getDB("test")}
];

collAndErrors.forEach(({coll, error, conn}) => {
    const collName = coll.getName();
    assert.commandFailedWithCode(
        coll.runCommand({find: collName, readConcern: {level: "snapshot"}}), error);

    assert.commandFailedWithCode(
        coll.runCommand(
            {aggregate: collName, pipeline: [], cursor: {}, readConcern: {level: "snapshot"}}),
        error);

    assert.commandFailedWithCode(
        coll.runCommand({distinct: collName, key: "_id", readConcern: {level: "snapshot"}}), error);

    // After starting a transaction with read concern snapshot, the following find command should
    // fail
    const session = conn.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase("config");
    session.startTransaction({readConcern: {level: 'snapshot'}});
    assert.commandFailedWithCode(sessionDB.runCommand({find: collName}), error);
});

st.stop();
replicaSet.stopSet();
