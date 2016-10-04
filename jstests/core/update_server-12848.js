// In SERVER-12848, it was shown that validation fails for certain types of updates
// because the validate code was attempting to validate field names in array. Mutable
// doesn't offer guarantees about the examined field name of array elements, only of the
// field name of array elements when serialized. This is a regression test to
// check that the new validation logic doesn't attempt to validate field names.

var res;
var t = db.update_server_12848;
t.drop();

var orig = {"_id": 1, "a": [1, []]};
res = t.insert(orig);
assert.writeOK(res, "insert");
assert.eq(orig, t.findOne());

res = t.update({"_id": 1}, {$addToSet: {"a.1": 1}});
assert.writeOK(res, "update");

var updated = {"_id": 1, "a": [1, [1]]};
assert.eq(updated, t.findOne());
