/**
 * Test that running explain() providing a collection UUID rather than collection name will fail
 * cleanly.
 * @tags: [
 *   no_selinux,
 *   assumes_stable_collection_uuid,
 * ]
 */
// Use our own database so that we're guaranteed the only collection is this one.
const explainDB = db.getSiblingDB("explain_uuid_db");

assert.commandWorked(explainDB.dropDatabase());

const coll = explainDB.explain_uuid;
assert.commandWorked(coll.insert({a: 1}));

const collInfos = explainDB.getCollectionInfos({name: coll.getName()});
assert.eq(collInfos.length, 1, collInfos);
const uuid = collInfos[0].info.uuid;

// Run a find explain looking up by UUID.
assert.commandFailedWithCode(explainDB.runCommand({explain: {find: uuid}}),
                             ErrorCodes.InvalidNamespace);

// Do similar for other commands.
assert.commandFailedWithCode(explainDB.runCommand({explain: {aggregate: uuid, cursor: {}}}),
                             ErrorCodes.TypeMismatch);

assert.commandFailedWithCode(explainDB.runCommand({explain: {count: uuid}}),
                             ErrorCodes.InvalidNamespace);

assert.commandFailedWithCode(explainDB.runCommand({explain: {distinct: uuid, key: "x"}}),
                             ErrorCodes.InvalidNamespace);

// When auth is enabled, running findAndModify with an invalid namespace will produce a special
// error during the auth check, rather than the generic 'InvalidNamespace' error.
const expectedCode = TestData.auth ? 17137 : ErrorCodes.InvalidNamespace;
assert.commandFailedWithCode(
    explainDB.runCommand({explain: {findAndModify: uuid, query: {a: 1}, remove: true}}),
    [expectedCode, ErrorCodes.BadValue]);

assert.commandFailedWithCode(
    explainDB.runCommand({explain: {delete: uuid, deletes: [{q: {}, limit: 1}]}}),
    ErrorCodes.BadValue);

assert.commandFailedWithCode(explainDB.runCommand({
    explain: {
        update: uuid,
        updates: [{
            q: {a: 1},
            u: {$set: {b: 1}},
        }]
    }
}),
                             ErrorCodes.BadValue);