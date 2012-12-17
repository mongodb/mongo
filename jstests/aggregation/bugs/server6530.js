// server-6530: disallow $near queries in $match operations
out = db.foo.aggregate({$match: {$near: [0,0]}});
assert.eq(out.code, 16424);

out = db.foo.aggregate({$match: {$nearSphere: [2,2]}});
assert.eq(out.code, 16426);
