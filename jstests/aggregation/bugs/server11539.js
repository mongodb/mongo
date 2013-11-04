// SERVER-11539 test with maximum-sized documents

var t = db.server11539;
t.drop();

var BSONObjMaxUserSize = db.isMaster().maxBsonObjectSize;
var maxBinarySize = BSONObjMaxUserSize
                  - 5 // bson obj overhead
                  - (1+4+4+1) // Binary overhead with field name "_id"
                  ;

var hex="";
for (var i = 0; i < maxBinarySize; i++) {
    hex += "00";
}
// This bug only happened with huge _ids since even with a NULL _id, there isn't enough room left in
// the BSON size threshold to fit a large enough string or bindata.
t.insert({_id: HexData(0, hex)});

res = t.aggregate([]).toArray(); // simply return all objects in collection
assert.eq(res.length, 1);
assert.eq(Object.bsonsize(res[0]), BSONObjMaxUserSize);
assert.eq(res[0]._id.len, maxBinarySize);
