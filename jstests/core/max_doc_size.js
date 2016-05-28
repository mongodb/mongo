var maxBsonObjectSize = db.isMaster().maxBsonObjectSize;
var docOverhead = Object.bsonsize({_id: new ObjectId(), x: ''});
var maxStrSize = maxBsonObjectSize - docOverhead;

var maxStr = 'a';
while (maxStr.length < maxStrSize)
    maxStr += 'a';

var coll = db.max_doc_size;

coll.drop();
var res = db.runCommand({insert: coll.getName(), documents: [{_id: new ObjectId(), x: maxStr}]});
assert(res.ok);
assert.eq(null, res.writeErrors);

coll.drop();
res = db.runCommand({
    update: coll.getName(),
    ordered: true,
    updates: [{q: {a: 1}, u: {_id: new ObjectId(), x: maxStr}, upsert: true}]
});
assert(res.ok);
assert.eq(null, res.writeErrors);

coll.drop();
var id = new ObjectId();
coll.insert({_id: id});
res = db.runCommand(
    {update: coll.getName(), ordered: true, updates: [{q: {_id: id}, u: {$set: {x: maxStr}}}]});
assert(res.ok);
assert.eq(null, res.writeErrors);

//
// Test documents over the size limit.
//

var overBigStr = maxStr + 'a';

coll.drop();
res = db.runCommand({insert: coll.getName(), documents: [{_id: new ObjectId(), x: overBigStr}]});
assert(res.ok);
assert.neq(null, res.writeErrors);

coll.drop();
res = db.runCommand({
    update: coll.getName(),
    ordered: true,
    updates: [{q: {a: 1}, u: {_id: new ObjectId(), x: overBigStr}, upsert: true}]
});
assert(res.ok);
assert.neq(null, res.writeErrors);

coll.drop();
id = new ObjectId();
coll.insert({_id: id});
res = db.runCommand(
    {update: coll.getName(), ordered: true, updates: [{q: {_id: id}, u: {$set: {x: overBigStr}}}]});
assert(res.ok);
assert.neq(null, res.writeErrors);
