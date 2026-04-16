/**
 * Ensures targeted host-list refresh does not race with replSet reconfig and use stale config.
 *
 * The test pauses setParameter-driven host cache refresh after reading ReplSetConfig, performs a
 * reconfig to advance version, then unpauses and verifies setParameter completes and the node
 * remains healthy.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_82,
 *   grpc_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {
        setParameter: {
            mirrorReads: tojson({targetedMirroring: {samplingRate: 1.0, tag: {Foo: "Bar"}}}),
        },
    },
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondaries()[0];

const hangDuringSetParameterUpdate = configureFailPoint(
    secondary.getDB("admin"),
    "mirrorMaestroHangDuringTargetedHostUpdate",
);

// This should block due to the failpoint above.
const awaitSetParameter = startParallelShell(function () {
    assert.commandWorked(
        db.getSiblingDB("admin").runCommand({
            setParameter: 1,
            mirrorReads: {targetedMirroring: {samplingRate: 1.0, tag: {Foo: "Baz"}}},
        }),
    );
}, secondary.port);

hangDuringSetParameterUpdate.wait();

let config = primary.getDB("local").system.replset.findOne();
const nextVersion = config.version + 1;
config.version = nextVersion;
assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

assert.soon(
    () => secondary.getDB("local").system.replset.findOne().version >= nextVersion,
    "Expected secondary to apply newer replSet config while setParameter thread is paused",
);

// Turning off the failpoint will unpause the setParameter operation
hangDuringSetParameterUpdate.off();

// Wait for set parameter to successfuly complete
awaitSetParameter();

// Verify that setParameter took effect.
const mirrorReadsParam = secondary.getDB("admin").runCommand({getParameter: 1, mirrorReads: 1}).mirrorReads;
assert.eq(mirrorReadsParam.targetedMirroring.tag, {Foo: "Baz"});

assert(checkProgram(secondary.pid).alive, "Expected secondary to remain alive after stale-config race window");

rst.awaitSecondaryNodes();
rst.stopSet();
