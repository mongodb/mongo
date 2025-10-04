// In SERVER-12848, it was shown that validation fails for certain types of updates
// because the validate code was attempting to validate field names in array. Mutable
// doesn't offer guarantees about the examined field name of array elements, only of the
// field name of array elements when serialized. This is a regression test to
// check that the new validation logic doesn't attempt to validate field names.

let res;
let t = db[jsTestName()];
t.drop();

let orig = {"_id": 1, "a": [1, []]};
res = t.insert(orig);
assert.commandWorked(res, "insert");
assert.eq(orig, t.findOne());

res = t.update({"_id": 1}, {$addToSet: {"a.1": 1}});
assert.commandWorked(res, "update");

let updated = {"_id": 1, "a": [1, [1]]};
assert.eq(updated, t.findOne());
