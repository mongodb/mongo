/**
 * This tests mongo shell functions bsonWoCompare & bsonBinaryEqual.
 */
(function() {
    'use strict';

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
        testObjectsAreEqual({a: 0}, {a: 0}, func, testFunc);
        testObjectsAreEqual({a: -5}, {a: -5}, func, testFunc);
        testObjectsAreEqual({a: 1}, {a: 1.0}, func, testFunc);
        testObjectsAreEqual({a: 1.1}, {a: 1.1}, func, testFunc);
        testObjectsAreEqual({a: 1.1}, {a: 1.10}, func, testFunc);
        var nl0 = new NumberLong("18014398509481984");
        var nl1 = new NumberLong("18014398509481985");
        testObjectsAreEqual({a: nl0}, {a: nl0}, func, testFunc);
        testObjectsAreNotEqual({a: nl0}, {a: nl1}, func, testFunc);

        // Test on key name.
        testObjectsAreNotEqual({a: 0}, {A: 0}, func, testFunc);

        // Tests on strings.
        testObjectsAreEqual({a: "abc"}, {a: "abc"}, func, testFunc);
        testObjectsAreNotEqual({a: "abc"}, {a: "aBc"}, func, testFunc);

        // Tests on boolean.
        testObjectsAreEqual({a: true}, {a: true}, func, testFunc);
        testObjectsAreNotEqual({a: true}, {a: false}, func, testFunc);
        testObjectsAreEqual({a: false}, {a: false}, func, testFunc);

        // Tests on date & timestamp.
        var d0 = new Date(0);
        var d1 = new Date(1);
        var ts0 = new Timestamp(0, 1);
        var ts1 = new Timestamp(1, 1);
        testObjectsAreEqual({a: d0}, {a: d0}, func, testFunc);
        testObjectsAreNotEqual({a: d1}, {a: d0}, func, testFunc);
        testObjectsAreNotEqual({a: d1}, {a: ts1}, func, testFunc);
        testObjectsAreEqual({a: ts0}, {a: ts0}, func, testFunc);
        testObjectsAreNotEqual({a: ts0}, {a: ts1}, func, testFunc);

        // Tests on regex.
        testObjectsAreEqual({a: /3/}, {a: /3/}, func, testFunc);
        testObjectsAreNotEqual({a: /3/}, {a: /3/i}, func, testFunc);

        // Tests on DBPointer.
        var dbp0 = new DBPointer("test", new ObjectId());
        var dbp1 = new DBPointer("test", new ObjectId());
        testObjectsAreEqual({a: dbp0}, {a: dbp0}, func, testFunc);
        testObjectsAreNotEqual({a: dbp0}, {a: dbp1}, func, testFunc);

        // Tests on JavaScript.
        var js0 = Function.prototype;
        var js1 = function() {};
        testObjectsAreEqual({a: js0}, {a: Function.prototype}, func, testFunc);
        testObjectsAreNotEqual({a: js0}, {a: js1}, func, testFunc);

        // Tests on arrays.
        testObjectsAreEqual({a: [0, 1]}, {a: [0, 1]}, func, testFunc);
        testObjectsAreNotEqual({a: [0, 1]}, {a: [0]}, func, testFunc);
        testObjectsAreNotEqual({a: [1, 0]}, {a: [0, 1]}, func, testFunc);

        // Tests on BinData & HexData.
        testObjectsAreEqual({a: new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")},
                            {a: new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")},
                            func,
                            testFunc);
        testObjectsAreEqual(
            {a: new BinData(0, "AAaa")}, {a: new BinData(0, "AAaa")}, func, testFunc);
        testObjectsAreNotEqual(
            {a: new BinData(0, "AAaa")}, {a: new BinData(0, "aaAA")}, func, testFunc);
        testObjectsAreEqual(
            {a: new HexData(0, "AAaa")}, {a: new HexData(0, "AAaa")}, func, testFunc);
        testObjectsAreEqual(
            {a: new HexData(0, "AAaa")}, {a: new HexData(0, "aaAA")}, func, testFunc);
        testObjectsAreNotEqual(
            {a: new HexData(0, "AAaa")}, {a: new BinData(0, "AAaa")}, func, testFunc);

        // Tests on ObjectId
        testObjectsAreEqual({a: new ObjectId("57d1b31cd311a43091fe592f")},
                            {a: new ObjectId("57d1b31cd311a43091fe592f")},
                            func,
                            testFunc);
        testObjectsAreNotEqual({a: new ObjectId("57d1b31cd311a43091fe592f")},
                               {a: new ObjectId("57d1b31ed311a43091fe5930")},
                               func,
                               testFunc);

        // Tests on miscellaneous types.
        testObjectsAreEqual({a: NaN}, {a: NaN}, func, testFunc);
        testObjectsAreEqual({a: null}, {a: null}, func, testFunc);
        testObjectsAreNotEqual({a: null}, {a: -null}, func, testFunc);
        testObjectsAreEqual({a: undefined}, {a: undefined}, func, testFunc);
        testObjectsAreNotEqual({a: undefined}, {a: null}, func, testFunc);
        testObjectsAreEqual({a: MinKey}, {a: MinKey}, func, testFunc);
        testObjectsAreEqual({a: MaxKey}, {a: MaxKey}, func, testFunc);
        testObjectsAreNotEqual({a: MinKey}, {a: MaxKey}, func, testFunc);

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
    testObjectsAreEqual({a: NaN}, {a: -NaN}, bsonWoCompareWrapper, "bsonWoCompare");
    testObjectsAreNotEqual({a: NaN}, {a: -NaN}, bsonBinaryEqual, "bsonBinaryEqual");
    testObjectsAreEqual({a: 1}, {a: NumberLong("1")}, bsonWoCompareWrapper, "bsonWoCompare");
    testObjectsAreNotEqual({a: 1}, {a: NumberLong("1")}, bsonBinaryEqual, "bsonBinaryEqual");
    testObjectsAreEqual({a: 1.0}, {a: NumberLong("1")}, bsonWoCompareWrapper, "bsonWoCompare");
    testObjectsAreNotEqual({a: 1.0}, {a: NumberLong("1")}, bsonBinaryEqual, "bsonBinaryEqual");
    testObjectsAreEqual(
        {a: NumberInt("1")}, {a: NumberLong("1")}, bsonWoCompareWrapper, "bsonWoCompare");
    testObjectsAreNotEqual(
        {a: NumberInt("1")}, {a: NumberLong("1")}, bsonBinaryEqual, "bsonBinaryEqual");
    testObjectsAreEqual(
        {a: NumberInt("1")}, {a: NumberDecimal("1.0")}, bsonWoCompareWrapper, "bsonWoCompare");
    testObjectsAreNotEqual(
        {a: NumberInt("1")}, {a: NumberDecimal("1.0")}, bsonBinaryEqual, "bsonBinaryEqual");
    testObjectsAreEqual(
        {a: NumberLong("1")}, {a: NumberDecimal("1.0")}, bsonWoCompareWrapper, "bsonWoCompare");
    testObjectsAreNotEqual(
        {a: NumberLong("1")}, {a: NumberDecimal("1.0")}, bsonBinaryEqual, "bsonBinaryEqual");

})();