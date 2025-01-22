// Tests that adding a field that only contains metadata does not cause a segmentation fault when
// grouping on the added field.
// Drop the old test collection, if any.
const coll = db[jsTestName()];
coll.drop();

// Insert some test documents into the collection.
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes and ale"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "more cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "bread"}));
assert.commandWorked(coll.insert({"_id": 4, "title": "some cakes"}));

// Create a text index on the documents.
assert.commandWorked(coll.createIndex({title: "text"}));

// Add a metadata only field in the aggregation pipeline and use that field in the $group _id.
const res = coll.aggregate([
                    {$match: {$text: {$search: "cake"}}},
                    {$addFields: {fooScore: {$meta: "textScore"}}},
                    {$group: {_id: "$fooScore", count: {$sum: 1}}}
                ])
                .itcount();

// Assert that the command worked.
assert.eq(2, res);
