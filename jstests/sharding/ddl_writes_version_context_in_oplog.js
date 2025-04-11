/**
 * Sharded DDL operations use a fixed FCV snapshot ("Operation FCV") over their lifetime, so that
 * their view of feature flags is isolated. This test checks that their oplog entries replicate
 * that FCV, which ensures secondaries also view the same feature flags when applying it.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
const db = st.s.getDB("test");

const collName = jsTestName();
const ns = db.getName() + "." + collName;

// Create a sharded collection. Since this is a sharded DDL, we expect it to use an Operation FCV.
assert.commandWorked(db.adminCommand({shardCollection: ns, key: {_id: 1}}));
st.awaitReplicationOnShards();

// Check that the version context has been replicated in the create op
const currentFCV =
    db.getSiblingDB("admin").system.version.findOne({_id: 'featureCompatibilityVersion'}).version;
const expectedVersionContext =
    FeatureFlagUtil.isPresentAndEnabled(db, "SnapshotFCVInDDLCoordinators") ? {OFCV: currentFCV}
                                                                            : undefined;

const filter = {
    op: "c",
    ns: db.getName() + '.$cmd',
    "o.create": collName
};
for (let node of st.rs0.nodes) {
    const oplogEntry = node.getDB("local").oplog.rs.find(filter).sort({$natural: -1}).next();
    assert.docEq(expectedVersionContext, oplogEntry.versionContext, tojson(oplogEntry));
}

st.stop();
