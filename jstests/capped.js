db=connect("test");
db.capped.drop();
db.createCollection("cptest", {capped:true, size:30000});
t = db.cptest;

t.save({x:1});
t.save({x:2});

assert( t.find().sort({$natural:1})[0].x == 1 );
assert( t.find().sort({$natural:-1})[0].x == 2 );

