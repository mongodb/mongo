// Reported as server-848.
db.server848.drop();

radius=0.0001;
center=[5,52];

db.server848.save({ "_id": 1, "loc" : { "x" : 4.9999, "y" : 52 } });
db.server848.save({ "_id": 2, "loc" : { "x" : 5, "y" : 52 } });
db.server848.save({ "_id": 3, "loc" : { "x" : 5.0001, "y" : 52 } });
db.server848.save({ "_id": 4, "loc" : { "x" : 5, "y" : 52.0001 } });
db.server848.save({ "_id": 5, "loc" : { "x" : 5, "y" : 51.9999 } });
db.server848.save({ "_id": 6, "loc" : { "x" : 4.9999, "y" : 52.0001 } });
db.server848.save({ "_id": 7, "loc" : { "x" : 5.0001, "y" : 52.0001 } });
db.server848.save({ "_id": 8, "loc" : { "x" : 4.9999, "y" : 51.9999 } });
db.server848.save({ "_id": 9, "loc" : { "x" : 5.0001, "y" : 51.9999 } });
db.server848.ensureIndex( { loc : "2d" } );
r=db.server848.find({"loc" : {"$within" : {"$center" : [center, radius]}}}, {_id:1});
assert.eq(5, r.count(), "A1"); 
// FIXME: surely code like this belongs in utils.js.
a=r.toArray();
x=[];
for (k in a) { x.push(a[k]["_id"]) }
x.sort()
assert.eq([1,2,3,4,5], x, "B1");
