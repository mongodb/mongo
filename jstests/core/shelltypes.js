// check that constructor also works without "new"
var a;
var b;
a = new ObjectId();
b = ObjectId(a.valueOf());
printjson(a);
assert.eq(tojson(a), tojson(b), "oid");

a = new DBRef("test", "theid");
b = DBRef(a.getRef(), a.getId());
printjson(a);
assert.eq(tojson(a), tojson(b), "dbref");

a = new DBPointer("test", new ObjectId());
b = DBPointer(a.getCollection(), a.getId());
printjson(a);
assert.eq(tojson(a), tojson(b), "dbpointer");

a = new Timestamp(10, 20);
b = Timestamp(a.t, a.i);
printjson(a);
assert.eq(tojson(a), tojson(b), "timestamp");

a = new BinData(3,"VQ6EAOKbQdSnFkRmVUQAAA==");
b = BinData(a.type, a.base64());
printjson(a);
assert.eq(tojson(a), tojson(b), "bindata");

a = new UUID("550e8400e29b41d4a716446655440000");
b = UUID(a.hex());
printjson(a);
assert.eq(tojson(a), tojson(b), "uuid");

a = new MD5("550e8400e29b41d4a716446655440000");
b = MD5(a.hex());
printjson(a);
assert.eq(tojson(a), tojson(b), "md5");

a = new HexData(4, "550e8400e29b41d4a716446655440000");
b = HexData(a.type, a.hex());
printjson(a);
assert.eq(tojson(a), tojson(b), "hexdata");

a = new NumberLong(100);
b = NumberLong(a.toNumber());
printjson(a);
assert.eq(tojson(a), tojson(b), "long");

a = new NumberInt(100);
b = NumberInt(a.toNumber());
printjson(a);
assert.eq(tojson(a), tojson(b), "int");

// ObjectId.fromDate

a = new ObjectId();
var timestampA = a.getTimestamp();
var dateA = new Date(timestampA.getTime());

// ObjectId.fromDate - invalid input types
assert.throws(function() { ObjectId.fromDate(undefined); }, null,
              "ObjectId.fromDate should error on undefined date" );

assert.throws(function() { ObjectId.fromDate(12345); }, null,
              "ObjectId.fromDate should error on numerical value" );

assert.throws(function() { ObjectId.fromDate(dateA.toISOString()); }, null,
              "ObjectId.fromDate should error on string value" );

// ObjectId.fromDate - Date
b = ObjectId.fromDate(dateA);
printjson(a);
assert.eq(tojson(a.getTimestamp()), tojson(b.getTimestamp()), "ObjectId.fromDate - Date");



// tojsonObject

// Empty object
assert.eq('{\n\t\n}', tojsonObject({}));
assert.eq('{  }', tojsonObject({}, '', true));
assert.eq('{\n\t\t\t\n\t\t}', tojsonObject({}, '\t\t'));

// Single field
assert.eq('{\n\t"a" : 1\n}', tojsonObject({a: 1}));
assert.eq('{ "a" : 1 }', tojsonObject({a: 1}, '', true));
assert.eq('{\n\t\t\t"a" : 1\n\t\t}', tojsonObject({a: 1}, '\t\t'));

// Multiple fields
assert.eq('{\n\t"a" : 1,\n\t"b" : 2\n}', tojsonObject({a: 1, b: 2}));
assert.eq('{ "a" : 1, "b" : 2 }', tojsonObject({a: 1, b: 2}, '', true));
assert.eq('{\n\t\t\t"a" : 1,\n\t\t\t"b" : 2\n\t\t}', tojsonObject({a: 1, b: 2}, '\t\t'));

// Nested fields
assert.eq('{\n\t"a" : 1,\n\t"b" : {\n\t\t"bb" : 2,\n\t\t"cc" : 3\n\t}\n}',
          tojsonObject({a: 1, b: {bb: 2, cc: 3}}));
assert.eq('{ "a" : 1, "b" : { "bb" : 2, "cc" : 3 } }',
          tojsonObject({a: 1, b: {bb: 2, cc: 3}}, '', true));
assert.eq('{\n\t\t\t"a" : 1,\n\t\t\t"b" : {\n\t\t\t\t"bb" : 2,\n\t\t\t\t"cc" : 3\n\t\t\t}\n\t\t}',
          tojsonObject({a: 1, b: {bb: 2, cc: 3}}, '\t\t'));

