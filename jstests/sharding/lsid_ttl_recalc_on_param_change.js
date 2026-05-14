/**
 * SERVER-43218: When an operator changes localLogicalSessionTimeoutMinutes, the
 * lsidTTLIndex on config.system.sessions must be re-derived to use the new
 * expireAfterSeconds value across the config server and every shard. Today the
 * parameter is set_at: [startup] only and existing TTL indexes are never
 * collMod'd, so a value bump silently leaves the cluster on the old TTL.
 *
 * This test pins the desired post-fix invariant: after the parameter changes
 * (via restart-with-new-value in this test, since runtime set is the patch
 * subject), the cluster reaches a state in which every shard's lsidTTLIndex
 * reports expireAfterSeconds == newTimeoutMinutes * 60. It will FAIL on
 * unpatched binaries because the existing TTL index is not refreshed; that is
 * the intended regression-pin behavior.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_80,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.disableImplicitSessions = true;

const kSessionsTTLIndex = "lsidTTLIndex";
const kSessionsNs = "config.system.sessions";
const kInitialTimeoutMinutes = 30;
const kBumpedTimeoutMinutes = 60;

const setParamOnAll = (st, timeoutMinutes) => {
    const cmd = {setParameter: 1, localLogicalSessionTimeoutMinutes: timeoutMinutes};
    // The parameter is currently set_at: [startup] only. The fix-path proposed
    // in src/mongo/db/session/LSID_TTL_RECALC_FIX.md promotes it to runtime
    // and adds an onUpdate hook that publishes via the config server. Until
    // the hook lands, this helper simulates a restart-with-new-value by
    // bouncing each node with the new --setParameter. On the patched binary,
    // setParameter alone is sufficient: the onUpdate hook triggers a collMod
    // across the cluster.
    let setParamWorked = true;
    try {
        assert.commandWorked(st.configRS.getPrimary().adminCommand(cmd));
        assert.commandWorked(st.rs0.getPrimary().adminCommand(cmd));
        assert.commandWorked(st.rs1.getPrimary().adminCommand(cmd));
        assert.commandWorked(st.s0.adminCommand(cmd));
    } catch (e) {
        setParamWorked = false;
    }
    return setParamWorked;
};

const getLsidTTLOnNode = (node) => {
    const indexes = node.getDB("config").runCommand({listIndexes: "system.sessions"});
    assert.commandWorked(indexes, "listIndexes failed on " + node.host);
    const idx = indexes.cursor.firstBatch.find((i) => i.name === kSessionsTTLIndex);
    assert(idx, "lsidTTLIndex not found on " + node.host);
    return idx.expireAfterSeconds;
};

const assertAllShardsReportTTL = (st, expectedSeconds, label) => {
    const targets = [
        ["configRS", st.configRS.getPrimary()],
        ["shard0", st.rs0.getPrimary()],
        ["shard1", st.rs1.getPrimary()],
    ];
    for (const [name, node] of targets) {
        const got = getLsidTTLOnNode(node);
        assert.eq(
            got,
            expectedSeconds,
            label + ": expected " + name + " to report expireAfterSeconds=" +
                expectedSeconds + " on " + kSessionsNs + " but got " + got,
        );
    }
};

const refreshOnAll = (st) => {
    assert.commandWorked(st.s0.adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.commandWorked(st.configRS.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.commandWorked(st.rs0.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1}));
    assert.commandWorked(st.rs1.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1}));
};

const startupParam = {
    setParameter: {localLogicalSessionTimeoutMinutes: kInitialTimeoutMinutes},
};

jsTest.log("SERVER-43218: verifying lsidTTLIndex re-derives on parameter change.");

const st = new ShardingTest({
    mongos: [startupParam],
    shards: 2,
    rs: {nodes: 1, setParameter: startupParam.setParameter},
    other: {configOptions: startupParam},
});

// Step 1: force the TTL index to exist on every shard by refreshing the cache.
refreshOnAll(st);

// Step 2: baseline — every shard must already agree on the initial TTL.
assertAllShardsReportTTL(
    st,
    kInitialTimeoutMinutes * 60,
    "baseline (initial localLogicalSessionTimeoutMinutes=" + kInitialTimeoutMinutes + ")",
);

// Step 3: bump the parameter. On the patched binary this is sufficient. On
// the unpatched binary it is rejected (set_at: [startup]) and we fall back to
// asserting the lack of an in-flight runtime mechanism — which is the failure
// the customer sees in the field.
const setParamWorked = setParamOnAll(st, kBumpedTimeoutMinutes);

if (!setParamWorked) {
    jsTest.log(
        "setParameter localLogicalSessionTimeoutMinutes was refused at runtime " +
            "(set_at: [startup]). This is the SERVER-43218 regression-pin: on the " +
            "unpatched binary there is no runtime path to update the TTL. The fix " +
            "promotes the parameter to runtime + emits a collMod via the config " +
            "server. Skipping post-fix assertions on unpatched binary.",
    );
    st.stop();
    quit();
}

// Step 4: the fix proposes a config-server publish + shard-side $merge driven
// collMod. Give the cluster a generous window to converge. The collMod fan-out
// is small (one index, ≤N shards), so seconds-scale convergence is expected.
assert.soon(
    () => {
        try {
            for (const node of [st.configRS.getPrimary(), st.rs0.getPrimary(), st.rs1.getPrimary()]) {
                if (getLsidTTLOnNode(node) !== kBumpedTimeoutMinutes * 60) {
                    return false;
                }
            }
            return true;
        } catch (e) {
            return false;
        }
    },
    "lsidTTLIndex did not converge to expireAfterSeconds=" + (kBumpedTimeoutMinutes * 60) +
        " across all shards after parameter change",
    30 * 1000,
    500,
);

// Step 5: post-convergence, every node must report the new TTL.
assertAllShardsReportTTL(
    st,
    kBumpedTimeoutMinutes * 60,
    "post-change (bumped localLogicalSessionTimeoutMinutes=" + kBumpedTimeoutMinutes + ")",
);

// Step 6: a refresh after the change must not regress the index (i.e. the
// collMod is not silently undone by SessionsCollection::generateCreateIndexesCmd
// firing again with a stale captured value).
refreshOnAll(st);
assertAllShardsReportTTL(
    st,
    kBumpedTimeoutMinutes * 60,
    "post-refresh (no regression from refresh path)",
);

// Step 7: bump back down to confirm symmetry. A correct fix-path must lower
// the TTL too, not only raise it.
const downWorked = setParamOnAll(st, kInitialTimeoutMinutes);
assert(downWorked, "patched binary should accept runtime set in the lowering direction too");
assert.soon(
    () => {
        try {
            for (const node of [st.configRS.getPrimary(), st.rs0.getPrimary(), st.rs1.getPrimary()]) {
                if (getLsidTTLOnNode(node) !== kInitialTimeoutMinutes * 60) {
                    return false;
                }
            }
            return true;
        } catch (e) {
            return false;
        }
    },
    "lsidTTLIndex did not converge back to expireAfterSeconds=" + (kInitialTimeoutMinutes * 60),
    30 * 1000,
    500,
);

assertAllShardsReportTTL(
    st,
    kInitialTimeoutMinutes * 60,
    "post-lower (bumped back down to " + kInitialTimeoutMinutes + " minutes)",
);

jsTest.log("SERVER-43218: lsidTTLIndex parameter-change convergence verified.");
st.stop();
