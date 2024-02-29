/**
 * SERVER-32125 Check that applyOps commands with collMod without UUID don't strip it
 *
 * @tags: [
 *  # The test runs commands that are not allowed with security token: applyOps.
 *  not_allowed_with_signed_security_token,
 *  requires_non_retryable_commands,
 *  # applyOps is not supported on mongos
 *  assumes_against_mongod_not_mongos,
 *  # applyOps uses the oplog that require replication support
 *  requires_replication,
 *  # Tenant migrations don't support applyOps.
 *  tenant_migration_incompatible,
 *  assumes_stable_collection_uuid,
 * ]
 */

const collName = "collmod_without_uuid";

function checkUUIDs() {
    let infos = db.getCollectionInfos();
    assert(infos.every((coll) => coll.name != collName || coll.info.uuid != undefined),
           "Not all collections have UUIDs: " + tojson({infos}));
}

db[collName].drop();
assert.commandWorked(db[collName].insert({}));
checkUUIDs();
let cmd = {applyOps: [{ns: "test.$cmd", op: "c", o: {collMod: collName}}]};
let res = db.runCommand(cmd);
assert.commandWorked(res, tojson(cmd));
checkUUIDs();
