/**
 * Confirms that:
 *  - Documents at the maximum BSON size limit can be written and read back.
 *  - Documents over the maximum BSON size limit cannot be written.
 *
 * @tags: [
 *     # The {$set: {x: maxStr}}} update takes multiple seconds to execute.
 *     operations_longer_than_stepdown_interval,
 *
 *     # In some passthroughs, specifically on tsan variants, this test can exhaust the number of
 *     # available retries or otherwise fail because of a long-running operation.
 *     tsan_incompatible,
 *     multiversion_incompatible,
 *
 *     # Creates memory pressure on secondaries in some passthroughs (esp. secondary_reads_passthrough)
 *     # under burn-in configuration.
 *     # TODO(SERVER-111328): Remove this tag after investigating memory resource issues.
 *     assumes_read_preference_unchanged,
 *  ]
 */

const maxBsonObjectSize = db.hello().maxBsonObjectSize;
const docOverhead = Object.bsonsize({_id: new ObjectId(), x: ""});
const maxStrSize = maxBsonObjectSize - docOverhead;
const maxStr = "a".repeat(maxStrSize);
const collNamePrefix = "jstests_max_doc_size_";
let coll;

//
// Test that documents at the size limit can be written and read back.
//
coll = db.getCollection(collNamePrefix + "insert_at_size_limit");
coll.drop();
assert.commandWorked(db.runCommand({insert: coll.getName(), documents: [{_id: new ObjectId(), x: maxStr}]}));
assert.eq(coll.find({}).itcount(), 1);

coll = db.getCollection(collNamePrefix + "upsert_at_size_limit");
coll.drop();
const objectId = new ObjectId();
assert.commandWorked(
    db.runCommand({
        update: coll.getName(),
        ordered: true,
        updates: [{q: {_id: objectId}, u: {_id: objectId, x: maxStr}, upsert: true}],
    }),
);
assert.eq(coll.find({}).itcount(), 1);

coll = db.getCollection(collNamePrefix + "update_at_size_limit");
coll.drop();

assert.commandWorked(coll.insert({_id: objectId}));
assert.commandWorked(
    db.runCommand({
        update: coll.getName(),
        ordered: true,
        updates: [{q: {_id: objectId}, u: {$set: {x: maxStr}}}],
    }),
);
assert.eq(coll.find({}).itcount(), 1);

//
// Test that documents over the size limit cannot be written.
//
const largerThanMaxString = maxStr + "a";

coll = db.getCollection(collNamePrefix + "insert_over_size_limit");
coll.drop();
assert.commandFailedWithCode(
    db.runCommand({insert: coll.getName(), documents: [{_id: new ObjectId(), x: largerThanMaxString}]}),
    ErrorCodes.BSONObjectTooLarge,
);

coll = db.getCollection(collNamePrefix + "upsert_over_size_limit");
coll.drop();
assert.commandFailedWithCode(
    db.runCommand({
        update: coll.getName(),
        ordered: true,
        updates: [{q: {_id: objectId}, u: {_id: objectId, x: largerThanMaxString}, upsert: true}],
    }),
    ErrorCodes.BSONObjectTooLarge,
);

coll = db.getCollection(collNamePrefix + "update_over_size_limit");
coll.drop();
assert.commandWorked(coll.insert({_id: objectId}));
assert.commandFailedWithCode(
    db.runCommand({
        update: coll.getName(),
        ordered: true,
        updates: [{q: {_id: objectId}, u: {$set: {x: largerThanMaxString}}}],
    }),
    ErrorCodes.BSONObjectTooLarge,
);
