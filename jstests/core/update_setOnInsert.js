// This tests that $setOnInsert works and allow setting the _id
t = db.update_setOnInsert;
var res;

function dotest(useIndex) {
    t.drop();
    if (useIndex) {
        t.ensureIndex({a: 1});
    }

    t.update({_id: 5}, {$inc: {x: 2}, $setOnInsert: {a: 3}}, true);
    assert.docEq({_id: 5, a: 3, x: 2}, t.findOne());

    t.update({_id: 5}, {$set: {a: 4}}, true);

    t.update({_id: 5}, {$inc: {x: 2}, $setOnInsert: {a: 3}}, true);
    assert.docEq({_id: 5, a: 4, x: 4}, t.findOne());
}

dotest(false);
dotest(true);

// Cases for SERVER-9958 -- Allow _id $setOnInsert during insert (if upsert:true, and not doc found)
t.drop();

res = t.update({_id: 1}, {$setOnInsert: {"_id.a": new Date()}}, true);
assert.writeError(res, "$setOnInsert _id.a worked");

res = t.update({"_id.a": 4}, {$setOnInsert: {"_id.b": 1}}, true);
assert.writeError(res, "$setOnInsert _id.a/b worked");

res = t.update({"_id.a": 4}, {$setOnInsert: {"_id": {a: 4, b: 1}}}, true);
assert.writeError(res, "$setOnInsert _id.a/a+b worked");
