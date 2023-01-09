/**
 * This tests mongo shell functions bsonWoCompare & bsonBinaryEqual.
 */

(function() {
'use strict';

var t = db.getCollection("bson");
t.drop();
function testObjectsAreEqual(obj1, obj2, equalityFunc, func_name) {
    var assert_msg = func_name + " " + tojson(obj1) + " " + tojson(obj2);
    assert(equalityFunc(obj1, obj2), assert_msg);
}

function testObjectsAreNotEqual(obj1, obj2, equalityFunc, func_name) {
    var assert_msg = func_name + " " + tojson(obj1) + " " + tojson(obj2);
    assert(!equalityFunc(obj1, obj2), assert_msg);
}

function runTests(func, testFunc) {
    // Tests on numbers.
    testObjectsAreEqual(0, 0, func, testFunc);
    testObjectsAreEqual(-5, -5, func, testFunc);
    testObjectsAreEqual(1.1, 1.1, func, testFunc);
    testObjectsAreEqual(1, 1, func, testFunc);
    testObjectsAreEqual(1.1, 1.10, func, testFunc);
    var nl0 = new NumberLong("18014398509481984");
    var nl1 = new NumberLong("18014398509481985");
    testObjectsAreEqual(nl0, nl0, func, testFunc);
    testObjectsAreNotEqual(nl0, nl1, func, testFunc);

    // Test on key name.
    t.insertMany([{a: 0}, {A: 0}]);
    testObjectsAreNotEqual(t.findOne({a: 0}), t.findOne({A: 0}), func, testFunc);

    // Tests on strings.
    testObjectsAreEqual("abc", "abc", func, testFunc);
    testObjectsAreNotEqual("abc", "aBc", func, testFunc);

    // Tests on boolean.
    testObjectsAreEqual(true, true, func, testFunc);
    testObjectsAreNotEqual(true, false, func, testFunc);
    testObjectsAreEqual(false, false, func, testFunc);

    // Tests on date & timestamp.
    var d0 = new Date(0);
    var d1 = new Date(1);
    var ts0 = new Timestamp(0, 1);
    var ts1 = new Timestamp(1, 1);
    testObjectsAreEqual(d0, d0, func, testFunc);
    testObjectsAreNotEqual(d0, d1, func, testFunc);
    testObjectsAreNotEqual(d1, ts1, func, testFunc);
    testObjectsAreEqual(ts0, ts0, func, testFunc);
    testObjectsAreNotEqual(ts0, ts1, func, testFunc);

    // Tests on regex.
    testObjectsAreEqual(/3/, /3/, func, testFunc);
    testObjectsAreNotEqual(/3/, /3/i, func, testFunc);

    // Tests on DBPointer.
    var dbp0 = new DBPointer("test", new ObjectId());
    var dbp1 = new DBPointer("test", new ObjectId());
    testObjectsAreEqual(dbp0, dbp0, func, testFunc);
    testObjectsAreNotEqual(dbp0, dbp1, func, testFunc);

    // Tests on JavaScript.
    var js0 = Function.prototype;
    var js1 = function() {};
    testObjectsAreEqual(js0, Function.prototype, func, testFunc);
    testObjectsAreNotEqual(js0, js1, func, testFunc);

    // Tests on arrays.
    testObjectsAreEqual([0, 1], [0, 1], func, testFunc);
    testObjectsAreNotEqual([0, 1], [0], func, testFunc);
    testObjectsAreNotEqual([1, 0], [0, 1], func, testFunc);

    // Tests on BinData & HexData.
    testObjectsAreEqual(new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq"),
                        new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq"),
                        func,
                        testFunc);
    testObjectsAreEqual(new BinData(0, "AAaa"), new BinData(0, "AAaa"), func, testFunc);
    testObjectsAreNotEqual(new BinData(0, "AAaa"), new BinData(0, "aaAA"), func, testFunc);

    testObjectsAreEqual(new HexData(0, "AAaa"), new HexData(0, "AAaa"), func, testFunc);
    testObjectsAreEqual(new HexData(0, "AAaa"), new HexData(0, "aaAA"), func, testFunc);
    testObjectsAreNotEqual(new HexData(0, "AAaa"), new BinData(0, "AAaa"), func, testFunc);

    // Tests on ObjectId
    testObjectsAreEqual(new ObjectId("57d1b31cd311a43091fe592f"),
                        new ObjectId("57d1b31cd311a43091fe592f"),
                        func,
                        testFunc);
    testObjectsAreNotEqual(new ObjectId("57d1b31cd311a43091fe592f"),
                           new ObjectId("57d1b31ed311a43091fe5930"),
                           func,
                           testFunc);

    // Tests on miscellaneous types.
    testObjectsAreEqual(NaN, NaN, func, testFunc);
    testObjectsAreEqual(null, null, func, testFunc);
    testObjectsAreNotEqual(null, -null, func, testFunc);
    testObjectsAreEqual(MinKey, MinKey, func, testFunc);
    testObjectsAreEqual(MaxKey, MaxKey, func, testFunc);
    testObjectsAreNotEqual(MinKey, MaxKey, func, testFunc);

    // Test on object ordering.
    testObjectsAreNotEqual({a: 1, b: 2}, {b: 2, a: 1}, func, testFunc);
}

// Create wrapper function for bsonWoCompare, such that it returns boolean result.
var bsonWoCompareWrapper = function(obj1, obj2) {
    return bsonWoCompare(obj1, obj2) === 0;
};

// Run the tests which work the same for both comparators.
runTests(bsonWoCompareWrapper, "bsonWoCompare");
runTests(bsonBinaryEqual, "bsonBinaryEqual");

// Run the tests which differ between comparators.
testObjectsAreEqual(NaN, -NaN, bsonWoCompareWrapper, "bsonWoCompare");
testObjectsAreNotEqual(NaN, -NaN, bsonBinaryEqual, "bsonBinaryEqual");
testObjectsAreEqual(1, NumberLong("1"), bsonWoCompareWrapper, "bsonWoCompare");
testObjectsAreNotEqual(1, NumberLong("1"), bsonBinaryEqual, "bsonBinaryEqual");
testObjectsAreEqual(1.0, NumberLong("1"), bsonWoCompareWrapper, "bsonWoCompare");
testObjectsAreNotEqual(1.0, NumberLong("1"), bsonBinaryEqual, "bsonBinaryEqual");
testObjectsAreEqual(NumberInt("1"), NumberLong("1"), bsonWoCompareWrapper, "bsonWoCompare");
testObjectsAreNotEqual(NumberInt("1"), NumberLong("1"), bsonBinaryEqual, "bsonBinaryEqual");
testObjectsAreEqual(NumberInt("1"), NumberDecimal("1.0"), bsonWoCompareWrapper, "bsonWoCompare");
testObjectsAreNotEqual(NumberInt("1"), NumberDecimal("1.0"), bsonBinaryEqual, "bsonBinaryEqual");
testObjectsAreEqual(NumberLong("1"), NumberDecimal("1.0"), bsonWoCompareWrapper, "bsonWoCompare");
testObjectsAreNotEqual(NumberLong("1"), NumberDecimal("1.0"), bsonBinaryEqual, "bsonBinaryEqual");
})();
