db.server6198.drop();

agg = db.server6198.aggregate({$group:{_id:null, "bar.baz": {$addToSet: "$foo"}}});
assert.eq(agg.code, 16414);
