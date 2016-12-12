t = db.bench_test3;
t.drop();

benchArgs = {
    ops: [{
        ns: t.getFullName(),
        op: "update",
        upsert: true,
        query: {_id: {"#RAND_INT": [0, 5, 4]}},
        update: {$inc: {x: 1}}
    }],
    parallel: 2,
    seconds: 10,
    host: db.getMongo().host
};

if (jsTest.options().auth) {
    benchArgs['db'] = 'admin';
    benchArgs['username'] = jsTest.options().authUser;
    benchArgs['password'] = jsTest.options().authPassword;
}

res = benchRun(benchArgs);
printjson(res);

var keys = [];
var totals = {};
db.bench_test3.find().sort({_id: 1}).forEach(function(z) {
    keys.push(z._id);
    totals[z._id] = z.x;
});
printjson(totals);
assert.eq([0, 4, 8, 12, 16], keys);
