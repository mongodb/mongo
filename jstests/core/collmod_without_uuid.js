/**
 * SERVER-32125 Check that applyOps commands with collMod without UUID don't strip it
 *
 * @tags: [
 *  requires_non_retryable_commands,
 *
 *  # applyOps uses the oplog that require replication support
 *  requires_replication
 * ]
 */

(function() {
"use strict";
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
})();
