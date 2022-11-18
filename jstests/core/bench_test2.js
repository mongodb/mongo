/**
 * The test runs commands that are not allowed with security token: benchRun.
 * @tags: [
 *   not_allowed_with_security_token,
 *   uses_multiple_connections,
 *   uses_parallel_shell,
 * ]
 */
(function() {
"use strict";

const t = db.bench_test2;
t.drop();

let docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({_id: i, x: 0});
}
assert.commandWorked(t.insert(docs));

const benchArgs = {
    ops: [{
        ns: t.getFullName(),
        op: "update",
        query: {_id: {"#RAND_INT": [0, 100]}},
        update: {$inc: {x: 1}},
        writeCmd: true
    }],
    parallel: 2,
    seconds: 1,
    host: db.getMongo().host
};

if (jsTest.options().auth) {
    benchArgs['db'] = 'admin';
    benchArgs['username'] = jsTest.options().authUser;
    benchArgs['password'] = jsTest.options().authPassword;
}

const res = benchRun(benchArgs);
printjson(res);

let sumsq = 0;
let sum = 0;

let min = 1000;
let max = 0;
t.find().forEach(function(z) {
    sum += z.x;
    sumsq += Math.pow((res.update / 100) - z.x, 2);
    min = Math.min(z.x, min);
    max = Math.max(z.x, max);
});

const avg = sum / 100;
const std = Math.sqrt(sumsq / 100);

print("Avg: " + avg);
print("Std: " + std);
print("Min: " + min);
print("Max: " + max);
})();
