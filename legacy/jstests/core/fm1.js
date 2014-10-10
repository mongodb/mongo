
t = db.fm1;
t.drop();

t.insert({foo:{bar:1}})
t.find({},{foo:1}).toArray();
t.find({},{'foo.bar':1}).toArray();
t.find({},{'baz':1}).toArray();
t.find({},{'baz.qux':1}).toArray();
t.find({},{'foo.qux':1}).toArray();


