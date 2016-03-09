
t = db.bench_test2;
t.drop();

for (i = 0; i < 100; i++)
    t.insert({_id: i, x: 0});

benchArgs = {
    ops: [{
        ns: t.getFullName(),
        op: "update",
        query: {_id: {"#RAND_INT": [0, 100]}},
        update: {$inc: {x: 1}}
    }],
    parallel: 2,
    seconds: 1,
    host: db.getMongo().host
};

if (jsTest.options().auth) {
    benchArgs['db'] = 'admin';
    benchArgs['username'] = jsTest.options().adminUser;
    benchArgs['password'] = jsTest.options().adminPassword;
}

res = benchRun(benchArgs);
printjson(res);

sumsq = 0;
sum = 0;

min = 1000;
max = 0;
t.find().forEach(function(z) {
    sum += z.x;
    sumsq += Math.pow((res.update / 100) - z.x, 2);
    min = Math.min(z.x, min);
    max = Math.max(z.x, max);
});

avg = sum / 100;
std = Math.sqrt(sumsq / 100);

print("Avg: " + avg);
print("Std: " + std);
print("Min: " + min);
print("Max: " + max);
