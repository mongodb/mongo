
t = db["users"];
t.remove( {} );

t.save({fn: "john", ln: "doe"})
t.ensureIndex({ln: 1});

assert( t.findOne({ln: "doe"}).fn == "john", "Cannot find right record" );
assert.eq( t.find({ln: "doe"}).explain().indexOnly, false, "Find using covered index but all fields are returned");
assert.eq( t.find({ln: "doe"}, {ln: 1}).explain().indexOnly, false, "Find using covered index but _id is returned");
assert.eq( t.find({ln: "doe"}, {ln: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
assert(t.validate().valid);

