// The {$set: {x: maxStr}}} update takes multiple seconds to execute.
// @tags: [
//   operations_longer_than_stepdown_interval,
//   multiversion_incompatible,
// ]

/**
 * Confirms that:
 *  - Documents at the maximum BSON size limit can be written and read back.
 *  - Documents over the maximum BSON size limit cannot be written.
 */
const maxBsonObjectSize = db.hello().maxBsonObjectSize;
const docOverhead = Object.bsonsize({_id: new ObjectId(), x: ''});
const maxStrSize = maxBsonObjectSize - docOverhead;
const maxStr = 'a'.repeat(maxStrSize);
const coll = db.max_doc_size;

//
// Test that documents at the size limit can be written and read back.
//
coll.drop();
assert.commandWorked(
    db.runCommand({insert: coll.getName(), documents: [{_id: new ObjectId(), x: maxStr}]}));
assert.eq(coll.find({}).itcount(), 1);

coll.drop();
const objectId = new ObjectId();
assert.commandWorked(db.runCommand({
    update: coll.getName(),
    ordered: true,
    updates: [{q: {_id: objectId}, u: {_id: objectId, x: maxStr}, upsert: true}]
}));
assert.eq(coll.find({}).itcount(), 1);

coll.drop();

assert.commandWorked(coll.insert({_id: objectId}));
assert.commandWorked(db.runCommand({
    update: coll.getName(),
    ordered: true,
    updates: [{q: {_id: objectId}, u: {$set: {x: maxStr}}}]
}));
assert.eq(coll.find({}).itcount(), 1);

//
// Test that documents over the size limit cannot be written.
//
const largerThanMaxString = maxStr + 'a';

coll.drop();
assert.commandFailedWithCode(
    db.runCommand(
        {insert: coll.getName(), documents: [{_id: new ObjectId(), x: largerThanMaxString}]}),
    ErrorCodes.BSONObjectTooLarge);

coll.drop();
assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    ordered: true,
    updates: [{q: {_id: objectId}, u: {_id: objectId, x: largerThanMaxString}, upsert: true}]
}),
                             17420);

coll.drop();
assert.commandWorked(coll.insert({_id: objectId}));
assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    ordered: true,
    updates: [{q: {_id: objectId}, u: {$set: {x: largerThanMaxString}}}]
}),
                             [17419, ErrorCodes.BSONObjectTooLarge]);
