// @tags: [does_not_support_stepdowns, requires_getmore]

t = db.mr_optim;
t.drop();

// We drop the output collection to ensure the test can be run multiple times successfully. We
// explicitly avoid using the DBCollection#drop() shell helper to avoid implicitly sharding the
// collection during the sharded_collections_jscore_passthrough.yml test suite when reading the
// results from the output collection in the reformat() function.
var res = db.runCommand({drop: "mr_optim_out"});
if (res.ok !== 1) {
    assert.commandFailedWithCode(res, ErrorCodes.NamespaceNotFound);
}

for (var i = 0; i < 1000; ++i) {
    assert.writeOK(t.save({a: Math.random(1000), b: Math.random(10000)}));
}

function m() {
    emit(this._id, 13);
}

function r(key, values) {
    return "bad";
}

function reformat(r) {
    var x = {};
    var cursor;
    if (r.results)
        cursor = r.results;
    else
        cursor = r.find().sort({_id: 1});
    cursor.forEach(function(z) {
        x[z._id] = z.value;
    });
    return x;
}

res = t.mapReduce(m, r, {out: "mr_optim_out"});
printjson(res);
x = reformat(res);
for (var key in x) {
    assert.eq(x[key], 13, "value is not equal to original, maybe reduce has run");
}
res.drop();

res = t.mapReduce(m, r, {out: {inline: 1}});
// printjson( res )
x2 = reformat(res);
res.drop();

assert.eq(x, x2, "object from inline and collection are not equal");

t.drop();
