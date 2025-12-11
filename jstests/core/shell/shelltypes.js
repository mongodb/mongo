import {it} from "jstests/libs/mochalite.js";

// check that constructor also works without "new"
it("ObjectId", () => {
    let a = new ObjectId();
    let b = ObjectId(a.valueOf());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "oid");
});

it("DBRef", () => {
    let a = new DBRef("test", "theid");
    let b = DBRef(a.getRef(), a.getId());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "dbref");

    a = new DBRef("test", "theid", "testdb");
    b = DBRef(a.getRef(), a.getId(), a.getDb());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "dbref");
});

it("DBPointer", () => {
    let a = new DBPointer("test", new ObjectId());
    let b = DBPointer(a.getCollection(), a.getId());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "dbpointer");
});

it("BinData", () => {
    let a = new BinData(3, "VQ6EAOKbQdSnFkRmVUQAAA==");
    let b = BinData(a.type, a.base64());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "bindata");
});

it("UUID", () => {
    let a = new UUID("550e8400e29b41d4a716446655440000");
    let b = UUID(a.hex());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "uuid");
});

it("MD5", () => {
    let a = new MD5("550e8400e29b41d4a716446655440000");
    let b = MD5(a.hex());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "md5");
});

it("HexData", () => {
    let a = new HexData(4, "550e8400e29b41d4a716446655440000");
    let b = HexData(a.type, a.hex());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "hexdata");
});

it("NumberLong", () => {
    let a = new NumberLong(100);
    let b = NumberLong(a.toNumber());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "long");
});

it("NumberInt", () => {
    let a = new NumberInt(100);
    let b = NumberInt(a.toNumber());
    printjson(a);
    assert.eq(tojson(a), tojson(b), "int");
});

it("ObjectId.fromDate", () => {
    let a = new ObjectId();
    let timestampA = a.getTimestamp();
    let dateA = new Date(timestampA.getTime());

    // ObjectId.fromDate - invalid input types
    assert.throws(() => ObjectId.fromDate(undefined), [], "ObjectId.fromDate should error on undefined date");

    assert.throws(() => ObjectId.fromDate(12345), [], "ObjectId.fromDate should error on numerical value");

    assert.throws(() => ObjectId.fromDate(dateA.toISOString()), [], "ObjectId.fromDate should error on string value");

    // SERVER-14623 dates less than or equal to 1978-07-04T21:24:15Z fail
    let checkFromDate = function (millis, expected, comment) {
        let oid = ObjectId.fromDate(new Date(millis));
        assert.eq(oid.valueOf(), expected, comment);
    };
    checkFromDate(Math.pow(2, 28) * 1000, "100000000000000000000000", "1978-07-04T21:24:16Z");
    checkFromDate(Math.pow(2, 28) * 1000 - 1, "0fffffff0000000000000000", "1978-07-04T21:24:15Z");
    checkFromDate(0, "000000000000000000000000", "start of epoch");

    // test date upper limit
    checkFromDate(Math.pow(2, 32) * 1000 - 1, "ffffffff0000000000000000", "last valid date");
    assert.throws(
        () => ObjectId.fromDate(new Date(Math.pow(2, 32) * 1000)),
        [],
        "ObjectId limited to 4 bytes for seconds",
    );

    // ObjectId.fromDate - Date
    let b = ObjectId.fromDate(dateA);
    printjson(a);
    assert.eq(tojson(a.getTimestamp()), tojson(b.getTimestamp()), "ObjectId.fromDate - Date");
});
