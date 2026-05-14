/**
 * SERVER-126118: After extracting `maxNumActiveUserIndexBuilds` and
 * `indexBuildMinAvailableDiskSpaceMB` from two_phase_index_build_knobs.idl
 * into shared_index_build_knobs.idl, both parameters must remain reachable
 * with identical defaults, types, and runtime-settability.
 *
 * This test pins the externally observable contract; if a future BUILD.bazel
 * edit drops shared_index_build_knobs_gen from index_build_knobs_idl, the
 * getParameter call below fails and surfaces the regression.
 *
 * @tags: [requires_persistence]
 */

const conn = MongoRunner.runMongod({});
const admin = conn.getDB("admin");

function getParam(name) {
    const out = assert.commandWorked(admin.runCommand({getParameter: 1, [name]: 1}));
    assert(name in out, `expected getParameter to return '${name}', got: ${tojson(out)}`);
    return out[name];
}

// Defaults documented in shared_index_build_knobs.idl
assert.eq(3,
          getParam("maxNumActiveUserIndexBuilds"),
          "maxNumActiveUserIndexBuilds default drifted");
assert.eq(500,
          getParam("indexBuildMinAvailableDiskSpaceMB"),
          "indexBuildMinAvailableDiskSpaceMB default drifted");

// Both knobs are set_at: [startup, runtime]; setParameter must succeed.
assert.commandWorked(
    admin.runCommand({setParameter: 1, maxNumActiveUserIndexBuilds: 5}));
assert.eq(5, getParam("maxNumActiveUserIndexBuilds"));

assert.commandWorked(
    admin.runCommand({setParameter: 1, indexBuildMinAvailableDiskSpaceMB: 1024}));
assert.eq(1024, getParam("indexBuildMinAvailableDiskSpaceMB"));

// Validators: gte:0 must reject negatives.
assert.commandFailed(
    admin.runCommand({setParameter: 1, maxNumActiveUserIndexBuilds: -1}));
assert.commandFailed(
    admin.runCommand({setParameter: 1, indexBuildMinAvailableDiskSpaceMB: -1}));

MongoRunner.stopMongod(conn);
