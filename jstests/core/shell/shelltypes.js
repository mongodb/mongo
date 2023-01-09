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

a = new DBRef("test", "theid", "testdb");
b = DBRef(a.getRef(), a.getId(), a.getDb());
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

assert.throws(function() {
    Timestamp(-2, 3);
}, [], "Timestamp time must not accept negative time");
assert.throws(function() {
    Timestamp(0, -1);
}, [], "Timestamp increment must not accept negative time");
assert.throws(function() {
    Timestamp(0x10000 * 0x10000, 0);
}, [], "Timestamp time must not accept values larger than 2**32 - 1");
assert.throws(function() {
    Timestamp(0, 0x10000 * 0x10000);
}, [], "Timestamp increment must not accept values larger than 2**32 - 1");

a = new Timestamp(0x80008000, 0x80008000 + 0.5);
b = Timestamp(a.t, Math.round(a.i));
printjson(a);
assert.eq(tojson(a), tojson(b), "timestamp");

a = new BinData(3, "VQ6EAOKbQdSnFkRmVUQAAA==");
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
assert.throws(function() {
    ObjectId.fromDate(undefined);
}, [], "ObjectId.fromDate should error on undefined date");

assert.throws(function() {
    ObjectId.fromDate(12345);
}, [], "ObjectId.fromDate should error on numerical value");

assert.throws(function() {
    ObjectId.fromDate(dateA.toISOString());
}, [], "ObjectId.fromDate should error on string value");

// SERVER-14623 dates less than or equal to 1978-07-04T21:24:15Z fail
var checkFromDate = function(millis, expected, comment) {
    var oid = ObjectId.fromDate(new Date(millis));
    assert.eq(oid.valueOf(), expected, comment);
};
checkFromDate(Math.pow(2, 28) * 1000, "100000000000000000000000", "1978-07-04T21:24:16Z");
checkFromDate((Math.pow(2, 28) * 1000) - 1, "0fffffff0000000000000000", "1978-07-04T21:24:15Z");
checkFromDate(0, "000000000000000000000000", "start of epoch");

// test date upper limit
checkFromDate((Math.pow(2, 32) * 1000) - 1, "ffffffff0000000000000000", "last valid date");
assert.throws(function() {
    ObjectId.fromDate(new Date(Math.pow(2, 32) * 1000));
}, [], "ObjectId limited to 4 bytes for seconds");

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
