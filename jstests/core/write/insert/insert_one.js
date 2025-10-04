/**
 * This test ensures that insertOne only accepts objects and that it doesn't insert any of the
 * object's prototype's methods.
 */
let col = db.insert_one_number;
col.drop();

assert.eq(col.find().itcount(), 0, "collection should be empty");

assert.throws(
    function () {
        col.insertOne(1);
    },
    [],
    "insertOne should only accept objects",
);

assert.eq(col.find().itcount(), 0, "collection should still be empty");

let result = col.insertOne({abc: "def"});
assert(result.acknowledged, "insertOne should succeed on documents");

assert.docEq(
    {_id: result.insertedId, abc: "def"},
    col.findOne({_id: result.insertedId}),
    "simple document not equal to collection find result",
);

let doc = new Number();
doc.x = 12;
assert("zeroPad" in doc, "number object should have 'zeroPad' in prototype");

result = col.insertOne(doc);
assert(result.acknowledged, "insertOne should succeed on documents");

assert(
    !("zeroPad" in col.findOne({_id: result.insertedId})),
    "inserted result should not have functions from the number object's prototype",
);

assert.docEq(
    {_id: result.insertedId, x: doc.x},
    col.findOne({_id: result.insertedId}),
    "document with prototype not equal to collection find result",
);
