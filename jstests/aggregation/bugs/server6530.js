// server-6530: disallow geo commmands in aggregation
out = db.foo.aggregate({$match: {$near: [0,0]}});
assert.eq(out.code, 16424);

out = db.foo.aggregate({$match: {$within: {$center: [[2,2], 10] }}});
assert.eq(out.code, 16425);

out = db.foo.aggregate({$match: {$nearSphere: [2,2]}});
assert.eq(out.code, 16426);
