
t = db.update8;
t.drop();

t.update({_id: 1, tags: {"$ne": "a"}}, {"$push": {tags: "a"}}, true);
assert.eq({_id: 1, tags: ["a"]}, t.findOne(), "A");

t.drop();
// SERVER-390
// t.update( { "x.y" : 1 } , { $inc : { i : 1 } } , true );
// printjson( t.findOne() );
