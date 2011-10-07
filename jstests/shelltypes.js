// check that constructor also works without "new"
var a, b;
a = new ObjectId();
b = ObjectId(a.toString());
assert.eq(printjson(a), printjson(b), "oid");

a = new DBRef("test", "theid");
b = DBRef(a["$ref"], a["$id"]);
assert.eq(printjson(a), printjson(b), "dbref");

a = new DBPointer("test", "theid");
b = DBPointer(a.ns, a.id);
assert.eq(printjson(a), printjson(b), "dbpointer");

a = new Timestamp(10, 20);
b = Timestamp(a.t, a.i);
assert.eq(printjson(a), printjson(b), "timestamp");

a = new BinData(3,"VQ6EAOKbQdSnFkRmVUQAAA==");
b = BinData(a.type, a.base64());
assert.eq(printjson(a), printjson(b), "bindata");

a = new UUID("550e8400e29b41d4a716446655440000");
b = UUID(a.hex());
assert.eq(printjson(a), printjson(b), "uuid");

a = new MD5("550e8400e29b41d4a716446655440000");
b = MD5(a.hex());
assert.eq(printjson(a), printjson(b), "md5");

a = new HexData(4, "550e8400e29b41d4a716446655440000");
b = HexData(a.type, a.hex());
assert.eq(printjson(a), printjson(b), "hexdata");

a = new NumberLong(100);
b = NumberLong(a.toNumber());
assert.eq(printjson(a), printjson(b), "long");

a = new NumberInt(100);
b = NumberInt(a.toNumber());
assert.eq(printjson(a), printjson(b), "int");

