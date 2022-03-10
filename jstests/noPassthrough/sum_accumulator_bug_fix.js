/**
 * Tests whether $sum accumulator incorrect result bug is fixed on both engines under FCV 6.0.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_sharding,
 * ]
 */
(function() {
'use strict';

(function testSumWhenSpillingOnClassicEngine() {
    const conn = MongoRunner.runMongod();

    const db = conn.getDB(jsTestName());
    const coll = db.spilling;

    for (let i = 0; i < 100; ++i) {
        assert.commandWorked(coll.insert([
            {k: i, n: 1e+34},
            {k: i, n: NumberDecimal("0.1")},
            {k: i, n: NumberDecimal("0.01")},
            {k: i, n: -1e+34}
        ]));
    }

    // Turns on the classical engine.
    db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true});

    const pipeline = [{$group: {_id: "$k", o: {$sum: "$n"}}}, {$group: {_id: "$o"}}];
    // The results when not spilling is the expected results.
    const expectedRes = coll.aggregate(pipeline).toArray();

    // Has the document source group spill.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: 1000}));

    // Makes sure that the document source group will spill.
    assert.commandFailedWithCode(
        coll.runCommand(
            {aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: false}),
        ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

    const classicSpillingRes = coll.aggregate(pipeline, {allowDiskUse: true}).toArray();
    assert.eq(classicSpillingRes, expectedRes);

    MongoRunner.stopMongod(conn);
}());

(function testOverTheWireDataFormatOnBothEngines() {
    const conn = MongoRunner.runMongod();

    const db = conn.getDB(jsTestName());
    assert.commandWorked(db.dropDatabase());
    const coll = db.spilling;
    const pipeline = [{$group: {_id: null, o: {$sum: "$n"}}}];
    const aggCmd = {
        aggregate: coll.getName(),
        pipeline: pipeline,
        needsMerge: true,
        fromMongos: true,
        cursor: {}
    };

    const verifyOverTheWireDataFormatOnBothEngines = (expectedRes) => {
        // Turns on the classical engine.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));
        const classicRes = assert.commandWorked(db.runCommand(aggCmd)).cursor.firstBatch;
        assert.eq(classicRes, expectedRes);

        // Turns off the classical engine.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));
        const sbeRes = assert.commandWorked(db.runCommand(aggCmd)).cursor.firstBatch;
        assert.eq(sbeRes, expectedRes);
    };

    assert.commandWorked(coll.insert({n: NumberInt(1)}));
    verifyOverTheWireDataFormatOnBothEngines([{
        _id: null,
        o: [
            NumberInt(16),  // The type id for NumberInt
            1.0,            // sum
            0.0             // addend
        ]
    }]);

    assert.commandWorked(coll.insert({n: NumberLong(1)}));
    verifyOverTheWireDataFormatOnBothEngines([{
        _id: null,
        o: [
            NumberInt(18),  // The type id for NumberLong
            2.0,            // sum
            0.0             // addend
        ]
    }]);

    assert.commandWorked(coll.insert({n: NumberLong("9223372036854775807")}));
    verifyOverTheWireDataFormatOnBothEngines([{
        _id: null,
        o: [
            NumberInt(18),          // The type id for NumberLong
            9223372036854775808.0,  // sum
            1.0                     // addend
        ]
    }]);

    // A double can always expresses 15 digits precisely. So, 1.0 + 0.00000000000001 is precisely
    // expressed by the 'addend' element.
    assert.commandWorked(coll.insert({n: 0.00000000000001}));
    verifyOverTheWireDataFormatOnBothEngines([{
        _id: null,
        o: [
            NumberInt(1),           // The type id for NumberDouble
            9223372036854775808.0,  // sum
            1.00000000000001        // addend
        ]
    }]);

    assert.commandWorked(coll.insert({n: NumberDecimal("1.0")}));
    verifyOverTheWireDataFormatOnBothEngines([{
        _id: null,
        o: [
            NumberInt(1),           // The type id for NumberDouble
            9223372036854775808.0,  // sum
            1.00000000000001,       // addend
            NumberDecimal("1.0")
        ]
    }]);

    assert(coll.drop());

    assert.commandWorked(coll.insert([{n: Number.MAX_VALUE}, {n: Number.MAX_VALUE}]));
    verifyOverTheWireDataFormatOnBothEngines([{
        _id: null,
        o: [
            NumberInt(1),  // The type id for NumberDouble
            Infinity,      // sum
            NaN            // addend
        ]
    }]);

    MongoRunner.stopMongod(conn);
}());

(function testShardedSumOnBothEngines() {
    const st = new ShardingTest({shards: 2});

    const db = st.getDB(jsTestName());
    assert.commandWorked(db.dropDatabase());
    const dbAtShard0 = st.shard0.getDB(jsTestName());
    const dbAtShard1 = st.shard1.getDB(jsTestName());

    // Makes sure that the test db is sharded.
    assert.commandWorked(st.s0.adminCommand({enableSharding: db.getName()}));

    let verifyShardedSumResultsOnBothEngine = (testDesc, coll, pipeline, expectedRes) => {
        // Turns to the classic engine at the shards.
        assert.commandWorked(
            dbAtShard0.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));
        assert.commandWorked(
            dbAtShard1.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));

        // Verifies that the classic engine's results are same as the expected results.
        const classicRes = coll.aggregate(pipeline).toArray();
        assert.eq(classicRes, expectedRes, testDesc);

        // Turns to the SBE engine at the shards.
        assert.commandWorked(
            dbAtShard0.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));
        assert.commandWorked(
            dbAtShard1.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));

        // Verifies that the SBE engine's results are same as the expected results.
        const sbeRes = coll.aggregate(pipeline).toArray();
        assert.eq(sbeRes, expectedRes, testDesc);
    };

    let shardCollectionByHashing = coll => {
        coll.drop();

        // Makes sure that the collection is sharded.
        assert.commandWorked(
            st.s0.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}));

        return coll;
    };

    let hashShardedColl = shardCollectionByHashing(db.partial_sum);
    let unshardedColl = db.partial_sum2;

    for (let i = 0; i < 3; ++i) {
        assert.commandWorked(hashShardedColl.insert([
            {k: i, n: 1e+34},
            {k: i, n: NumberDecimal("0.1")},
            {k: i, n: NumberDecimal("0.01")},
            {k: i, n: -1e+34}
        ]));
        assert.commandWorked(unshardedColl.insert([
            {k: i, n: 1e+34},
            {k: i, n: NumberDecimal("0.1")},
            {k: i, n: NumberDecimal("0.01")},
            {k: i, n: -1e+34}
        ]));
    }

    const pipeline = [{$group: {_id: "$k", s: {$sum: "$n"}}}, {$group: {_id: "$s"}}];

    // The results on an unsharded collection is the expected results.
    const expectedRes = unshardedColl.aggregate(pipeline).toArray();
    verifyShardedSumResultsOnBothEngine(
        "Sharded sum for mixed data by which only decimal sum survive",
        hashShardedColl,
        pipeline,
        expectedRes);

    const int32Max = 2147483647;
    const numberIntMax = NumberInt(int32Max);
    const numberLongMax = NumberLong("9223372036854775807");
    const verySmallDecimal = NumberDecimal("1e-15");
    const veryLargeDecimal = NumberDecimal("1e+33");

    // This value is precisely representable by a double.
    const doubleClosestToLongMax = 9223372036854775808.0;
    [{
        testDesc: "No documents evaluated",
        inputs: [{}],
        expectedRes: [{_id: null, o: NumberInt(0)}]
    },
     {
         testDesc: "An int",
         inputs: [{n: NumberInt(10)}],
         expectedRes: [{_id: null, o: NumberInt(10)}]
     },
     {
         testDesc: "a long",
         inputs: [{n: NumberLong(10)}],
         expectedRes: [{_id: null, o: NumberLong(10)}]
     },
     {testDesc: "A double", inputs: [{n: 10.0}], expectedRes: [{_id: null, o: 10.0}]},
     {
         testDesc: "A long that cannot be expressed as an int",
         inputs: [{n: NumberLong("60000000000")}],
         expectedRes: [{_id: null, o: NumberLong("60000000000")}]
     },
     {
         testDesc: "A non integer valued double",
         inputs: [{n: 7.5}],
         expectedRes: [{_id: null, o: 7.5}]
     },
     {testDesc: "A nan double", inputs: [{n: NaN}], expectedRes: [{_id: null, o: NaN}]},
     {testDesc: "A -nan double", inputs: [{n: -NaN}], expectedRes: [{_id: null, o: -NaN}]},
     {
         testDesc: "A infinity double",
         inputs: [{n: Infinity}],
         expectedRes: [{_id: null, o: Infinity}]
     },
     {
         testDesc: "A -infinity double",
         inputs: [{n: -Infinity}],
         expectedRes: [{_id: null, o: -Infinity}]
     },
     {
         testDesc: "Two ints are summed",
         inputs: [{n: NumberInt(4)}, {n: NumberInt(5)}],
         expectedRes: [{_id: null, o: NumberInt(9)}]
     },
     {
         testDesc: "An int and a long",
         inputs: [{n: NumberInt(4)}, {n: NumberLong(5)}],
         expectedRes: [{_id: null, o: NumberLong(9)}]
     },
     {
         testDesc: "Two longs",
         inputs: [{n: NumberLong(4)}, {n: NumberLong(5)}],
         expectedRes: [{_id: null, o: NumberLong(9)}]
     },
     {
         testDesc: "An int and a double",
         inputs: [{n: NumberInt(4)}, {n: 5.5}],
         expectedRes: [{_id: null, o: 9.5}]
     },
     {
         testDesc: "A long and a double",
         inputs: [{n: NumberLong(4)}, {n: 5.5}],
         expectedRes: [{_id: null, o: 9.5}]
     },
     {testDesc: "Two doubles", inputs: [{n: 2.5}, {n: 5.5}], expectedRes: [{_id: null, o: 8.0}]},
     {
         testDesc: "An int, a long, and a double",
         inputs: [{n: NumberInt(5)}, {n: NumberLong(99)}, {n: 0.2}],
         expectedRes: [{_id: null, o: 104.2}]
     },
     {
         testDesc: "Two decimals",
         inputs: [{n: NumberDecimal("-10.100")}, {n: NumberDecimal("20.200")}],
         expectedRes: [{_id: null, o: NumberDecimal("10.100")}]
     },
     {
         testDesc: "Two longs and a decimal",
         inputs: [{n: NumberLong(10)}, {n: NumberLong(10)}, {n: NumberDecimal("10.000")}],
         expectedRes: [{_id: null, o: NumberDecimal("30.000")}]
     },
     {
         testDesc: "A double and a decimal",
         inputs: [{n: 2.5}, {n: NumberDecimal("2.5")}],
         expectedRes: [{_id: null, o: NumberDecimal("5.0")}]
     },
     {
         testDesc: "An int, long, double and decimal",
         inputs: [{n: NumberInt(10)}, {n: NumberLong(10)}, {n: 10.5}, {n: NumberDecimal("9.6")}],
         expectedRes: [{_id: null, o: NumberDecimal("40.1")}]
     },
     {
         testDesc: "A long max and a very small decimal resulting in 34 digits",
         inputs: [{n: numberLongMax}, {n: verySmallDecimal}],
         expectedRes: [{_id: null, o: NumberDecimal("9223372036854775807.000000000000001")}]
     },
     {
         testDesc: "A long and a very large decimal resulting in 34 digits",
         inputs: [{n: NumberLong(1)}, {n: veryLargeDecimal}],
         expectedRes: [{_id: null, o: NumberDecimal("1000000000000000000000000000000001")}]
     },
     {
         testDesc:
             "The double closest to the long max and a very small decimal resulting in 34 digits",
         inputs: [{n: doubleClosestToLongMax}, {n: verySmallDecimal}],
         expectedRes: [{_id: null, o: NumberDecimal("9223372036854775808.000000000000001")}]
     },
     {
         testDesc: "A double and a very large decimal resulting in 34 digits",
         inputs: [{n: 1.0}, {n: veryLargeDecimal}],
         expectedRes: [{_id: null, o: NumberDecimal("1000000000000000000000000000000001")}]
     },
     {
         testDesc: "A negative value is summed",
         inputs: [{n: NumberInt(5)}, {n: -8.5}],
         expectedRes: [{_id: null, o: -3.5}]
     },
     {
         testDesc: "A long and a negative int are summed",
         inputs: [{n: NumberLong(5)}, {n: NumberInt(-6)}],
         expectedRes: [{_id: null, o: NumberLong(-1)}]
     },
     {
         testDesc: "Two ints do not overflow",
         inputs: [{n: numberIntMax}, {n: NumberInt(10)}],
         expectedRes: [{_id: null, o: NumberLong(int32Max + 10)}]
     },
     {
         testDesc: "Two negative ints do not overflow",
         inputs: [{n: NumberInt(-int32Max)}, {n: NumberInt(-10)}],
         expectedRes: [{_id: null, o: NumberLong(-int32Max - 10)}]
     },
     {
         testDesc: "An int and a long do not trigger an int overflow",
         inputs: [{n: numberIntMax}, {n: NumberLong(1)}],
         expectedRes: [{_id: null, o: NumberLong(int32Max + 1)}]
     },
     {
         testDesc: "An int and a double do not trigger an int overflow",
         inputs: [{n: numberIntMax}, {n: 1.0}],
         expectedRes: [{_id: null, o: int32Max + 1.0}]
     },
     {
         testDesc: "An int and a long overflow into a double",
         inputs: [{n: NumberInt(1)}, {n: numberLongMax}],
         expectedRes: [{_id: null, o: doubleClosestToLongMax}]
     },
     {
         testDesc: "Two longs overflow into a double",
         inputs: [{n: numberLongMax}, {n: numberLongMax}],
         expectedRes: [{_id: null, o: doubleClosestToLongMax * 2}]
     },
     {
         testDesc: "A long and a double do not trigger a long overflow",
         inputs: [{n: numberLongMax}, {n: 1.0}],
         expectedRes: [{_id: null, o: doubleClosestToLongMax}]
     },
     {
         testDesc: "Two doubles overflow to infinity",
         inputs: [{n: Number.MAX_VALUE}, {n: Number.MAX_VALUE}],
         expectedRes: [{_id: null, o: Infinity}]
     },
     {
         testDesc: "Two large integers do not overflow if a double is added later",
         inputs: [{n: numberLongMax}, {n: numberLongMax}, {n: 1.0}],
         expectedRes: [{_id: null, o: doubleClosestToLongMax * 2}]
     },
     {
         testDesc: "An int and a NaN double",
         inputs: [{n: NumberInt(4)}, {n: NaN}],
         expectedRes: [{_id: null, o: NaN}]
     },
     {
         testDesc: "Null values are ignored",
         inputs: [{n: NumberInt(5)}, {n: null}],
         expectedRes: [{_id: null, o: NumberInt(5)}]
     },
     {
         testDesc: "Missing values are ignored",
         inputs: [{n: NumberInt(9)}, {}],
         expectedRes: [{_id: null, o: NumberInt(9)}]
     }].forEach(({testDesc, inputs, expectedRes}) => {
        hashShardedColl.drop();
        assert.commandWorked(hashShardedColl.insert(inputs));

        verifyShardedSumResultsOnBothEngine(
            testDesc, hashShardedColl, [{$group: {_id: null, o: {$sum: "$n"}}}], expectedRes);
    });

    st.stop();
}());
}());
