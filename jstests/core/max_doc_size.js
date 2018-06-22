/**
 * Confirms that:
 *  - Documents at the maximum BSON size limit can be written and read back.
 *  - Documents over the maximum BSON size limit cannot be written.
 */
(function() {
    'use strict';

    const maxBsonObjectSize = db.isMaster().maxBsonObjectSize;
    const docOverhead = Object.bsonsize({_id: new ObjectId(), x: ''});
    const maxStrSize = maxBsonObjectSize - docOverhead;
    const maxStr = 'a'.repeat(maxStrSize);
    const coll = db.max_doc_size;

    //
    // Test that documents at the size limit can be written and read back.
    //
    coll.drop();
    assert.writeOK(
        db.runCommand({insert: coll.getName(), documents: [{_id: new ObjectId(), x: maxStr}]}));
    assert.eq(coll.find({}).itcount(), 1);

    coll.drop();
    assert.commandWorked(db.runCommand({
        update: coll.getName(),
        ordered: true,
        updates: [{q: {a: 1}, u: {_id: new ObjectId(), x: maxStr}, upsert: true}]
    }));
    assert.eq(coll.find({}).itcount(), 1);

    coll.drop();
    const objectId = new ObjectId();
    assert.writeOK(coll.insert({_id: objectId}));
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
    let res = db.runCommand(
        {insert: coll.getName(), documents: [{_id: new ObjectId(), x: largerThanMaxString}]});
    assert(res.ok);
    assert.neq(null, res.writeErrors);

    coll.drop();
    res = db.runCommand({
        update: coll.getName(),
        ordered: true,
        updates: [{q: {a: 1}, u: {_id: new ObjectId(), x: largerThanMaxString}, upsert: true}]
    });
    assert(res.ok);
    assert.neq(null, res.writeErrors);

    coll.drop();
    assert.writeOK(coll.insert({_id: objectId}));
    res = db.runCommand({
        update: coll.getName(),
        ordered: true,
        updates: [{q: {_id: objectId}, u: {$set: {x: largerThanMaxString}}}]
    });
    assert(res.ok);
    assert.neq(null, res.writeErrors);
})();
