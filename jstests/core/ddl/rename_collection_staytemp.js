/*
 * @tags: [
 *   # The test runs commands that are not allowed with security token: applyOps.
 *   not_allowed_with_signed_security_token,
 *   requires_non_retryable_commands,
 *   requires_replication,
 *   # applyOps is not supported on mongos
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 * ]
 */

let orig = "rename_stayTemp_orig";
let dest = "rename_stayTemp_dest";

db[orig].drop();
db[dest].drop();

function ns(coll) {
    return db[coll].getFullName();
}

function istemp(name) {
    let result = db.runCommand("listCollections", {filter: {name: name}});
    assert(result.ok);
    let collections = new DBCommandCursor(db, result).toArray();
    assert.eq(1, collections.length);
    return collections[0].options.temp ? true : false;
}

assert.commandWorked(db.runCommand({applyOps: [{op: "c", ns: db.getName() + ".$cmd", o: {create: orig, temp: 1}}]}));
assert(istemp(orig));

db.adminCommand({renameCollection: ns(orig), to: ns(dest)});
assert(!istemp(dest));

db[dest].drop();

assert.commandWorked(db.runCommand({applyOps: [{op: "c", ns: db.getName() + ".$cmd", o: {create: orig, temp: 1}}]}));
assert(istemp(orig));

db.adminCommand({renameCollection: ns(orig), to: ns(dest), stayTemp: true});
assert(istemp(dest));
