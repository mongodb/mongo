//Test applyops upsert flag SERVER-7452

var t = db.apply_ops2;
t.drop();

assert.eq(0, t.find().count(), "test collection not empty");

t.insert({_id:1, x:"init"});

//alwaysUpsert = true
print("Testing applyOps with alwaysUpsert = true");

var res = db.runCommand({ applyOps: [
    {
        op: "u",
        ns: t.getFullName(),
        o2 : { _id: 1 },
        o: { $set: { x: "upsert=true existing" }}
    },
    {
        op: "u",
        ns: t.getFullName(),
        o2: { _id: 2 },
        o: { $set : { x: "upsert=true non-existing" }}
    }], alwaysUpsert: true });

assert.eq(true, res.results[0], "upsert = true, existing doc update failed");
assert.eq(true, res.results[1], "upsert = true, nonexisting doc not upserted");
assert.eq(2, t.find().count(), "2 docs expected after upsert");

//alwaysUpsert = false
print("Testing applyOps with alwaysUpsert = false");

res = db.runCommand({ applyOps: [
    {
        op: "u",
        ns: t.getFullName(),
        o2: { _id: 1 },
        o: { $set : { x: "upsert=false existing" }}
    },
    {
        op: "u",
        ns: t.getFullName(),
        o2: { _id: 3 },
        o: { $set: { x: "upsert=false non-existing" }}
    }], alwaysUpsert: false });

assert.eq(true, res.results[0], "upsert = false, existing doc update failed");
assert.eq(false, res.results[1], "upsert = false, nonexisting doc upserted");
assert.eq(2, t.find().count(), "2 docs expected after upsert failure");

//alwaysUpsert not specified, should default to true
print("Testing applyOps with default alwaysUpsert");

res = db.runCommand({ applyOps: [
    {
        op: "u",
        ns: t.getFullName(),
        o2: { _id: 1 },
        o: { $set: { x: "upsert=default existing" }}
    },
    {
        op: "u",
        ns: t.getFullName(),
        o2: { _id: 4 },
        o: { $set: { x: "upsert=defaults non-existing" }}
    }]});

assert.eq(true, res.results[0], "default upsert, existing doc update failed");
assert.eq(true, res.results[1], "default upsert, nonexisting doc not upserted");
assert.eq(3, t.find().count(), "2 docs expected after upsert failure");
