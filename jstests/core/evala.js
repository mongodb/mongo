
t = db.evala;
t.drop();

t.save({x: 5});

assert.eq(5, db.eval("function(){ return db.evala.findOne().x; }"), "A");
assert.eq(5, db.eval("/* abc */function(){ return db.evala.findOne().x; }"), "B");
