// check that constructor also works without "new"
var a, b;
a = new ObjectId();
b = ObjectId(a.toString());
assert.eq(tojson(a), tojson(b), "oid");

a = new DBRef("test", "theid");
b = DBRef(a["$ref"], a["$id"]);
assert.eq(tojson(a), tojson(b), "dbref");

a = new DBPointer("test", new ObjectId());
b = DBPointer(a.ns, a.id);
assert.eq(tojson(a), tojson(b), "dbpointer");

a = new Timestamp(10, 20);
b = Timestamp(a.t, a.i);
assert.eq(tojson(a), tojson(b), "timestamp");

a = new BinData(3,"VQ6EAOKbQdSnFkRmVUQAAA==");
b = BinData(a.type, a.base64());
assert.eq(tojson(a), tojson(b), "bindata");

a = new UUID("550e8400e29b41d4a716446655440000");
b = UUID(a.hex());
assert.eq(tojson(a), tojson(b), "uuid");

a = new MD5("550e8400e29b41d4a716446655440000");
b = MD5(a.hex());
assert.eq(tojson(a), tojson(b), "md5");

a = new HexData(4, "550e8400e29b41d4a716446655440000");
b = HexData(a.type, a.hex());
assert.eq(tojson(a), tojson(b), "hexdata");

a = new NumberLong(100);
b = NumberLong(a.toNumber());
assert.eq(tojson(a), tojson(b), "long");

a = new NumberInt(100);
b = NumberInt(a.toNumber());
assert.eq(tojson(a), tojson(b), "int");

