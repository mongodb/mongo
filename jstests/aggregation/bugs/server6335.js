// server-6335: don't allow $where clauses in a $match
out = db.foo.aggregate({$match: {$where: "return true"}})
assert.eq(out.code, 16395)

out = db.foo.aggregate({$match: {$and:[{$where: "return true"}]}})
assert.eq(out.code, 16395)
