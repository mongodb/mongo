
t = db.geoc;
t.drop();

N = 1000;

for (var i = 0; i < N; i++)
    t.insert({loc: [100 + Math.random(), 100 + Math.random()], z: 0});
for (var i = 0; i < N; i++)
    t.insert({loc: [0 + Math.random(), 0 + Math.random()], z: 1});
for (var i = 0; i < N; i++)
    t.insert({loc: [-100 + Math.random(), -100 + Math.random()], z: 2});

t.ensureIndex({loc: '2d'});

function test(z, l) {
    assert.lt(
        0, t.find({loc: {$near: [100, 100]}, z: z}).limit(l).itcount(), "z: " + z + " l: " + l);
}

test(1, 1);
test(1, 2);
test(2, 2);
test(2, 10);
test(2, 1000);
test(2, 100000);
test(2, 10000000);
