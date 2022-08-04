/**
 * Tests whether $sum/$avg accumulator incorrect result bug is fixed on both engines.
 *
 * @tags: [
 *  requires_sharding,
 * ]
 */
(function() {
'use strict';

(function testAccumulatorWhenSpillingOnClassicEngine() {
    const verifyAccumulatorSpillingResult = (testDesc, accSpec) => {
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
        assert.commandWorked(db.adminCommand(
            {setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

        const pipeline = [{$group: {_id: "$k", o: accSpec}}, {$group: {_id: "$o"}}];

        // The results when not spilling is the expected results.
        const expectedRes = coll.aggregate(pipeline).toArray();

        // Has the document source group spill.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: 1000}),
            testDesc);

        // Makes sure that the document source group will spill.
        assert.commandFailedWithCode(
            coll.runCommand(
                {aggregate: coll.getName(), pipeline: pipeline, cursor: {}, allowDiskUse: false}),
            ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
            testDesc);

        const classicSpillingRes = coll.aggregate(pipeline, {allowDiskUse: true}).toArray();
        assert.eq(classicSpillingRes, expectedRes, testDesc);

        MongoRunner.stopMongod(conn);
    };

    verifyAccumulatorSpillingResult("Verifying $sum spilling bug is fixed on the classic engine",
                                    {$sum: "$n"});

    verifyAccumulatorSpillingResult("Verifying $avg spilling bug is fixed on the classic engine",
                                    {$avg: "$n"});
}());

(function testOverTheWireDataFormatOnBothEngines() {
    const conn = MongoRunner.runMongod();

    const db = conn.getDB(jsTestName());
    const coll = db.spilling;

    const verifyOverTheWireDataFormatOnBothEngines = (testDesc, pipeline, expectedRes) => {
        const aggCmd = {
            aggregate: coll.getName(),
            pipeline: pipeline,
            needsMerge: true,
            fromMongos: true,
            cursor: {}
        };

        // Turns on the classical engine.
        assert.commandWorked(db.adminCommand(
            {setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
        const classicRes = assert.commandWorked(db.runCommand(aggCmd)).cursor.firstBatch;
        assert.eq(classicRes, expectedRes, testDesc);

        // Turns off the classical engine.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
        const sbeRes = assert.commandWorked(db.runCommand(aggCmd)).cursor.firstBatch;
        assert.eq(sbeRes, expectedRes, testDesc);
    };

    (function testOverTheWireDataFormat() {
        const pipelineWithSum = [{$group: {_id: null, o: {$sum: "$n"}}}];
        const pipelineWithAvg = [{$group: {_id: null, o: {$avg: "$n"}}}];

        assert.commandWorked(coll.insert({n: NumberInt(1)}));
        let expectedPartialSum = [
            NumberInt(16),  // The type id for NumberInt
            1.0,            // sum
            0.0             // addend
        ];
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial sum of an int", pipelineWithSum, [{_id: null, o: expectedPartialSum}]);
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial avg of an int",
            pipelineWithAvg,
            [{_id: null, o: {count: NumberLong(1), ps: expectedPartialSum}}]);

        assert.commandWorked(coll.insert({n: NumberLong(1)}));
        expectedPartialSum = [
            NumberInt(18),  // The type id for NumberLong
            2.0,            // sum
            0.0             // addend
        ];
        verifyOverTheWireDataFormatOnBothEngines("Partial sum of an int and a long",
                                                 pipelineWithSum,
                                                 [{_id: null, o: expectedPartialSum}]);
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial avg of an int and a long",
            pipelineWithAvg,
            [{_id: null, o: {count: NumberLong(2), ps: expectedPartialSum}}]);

        assert.commandWorked(coll.insert({n: NumberLong("9223372036854775807")}));
        expectedPartialSum = [
            NumberInt(18),          // The type id for NumberLong
            9223372036854775808.0,  // sum
            1.0                     // addend
        ];
        verifyOverTheWireDataFormatOnBothEngines("Partial sum of an int/a long/the long max",
                                                 pipelineWithSum,
                                                 [{_id: null, o: expectedPartialSum}]);
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial avg of an int/a long/the long max",
            pipelineWithAvg,
            [{_id: null, o: {count: NumberLong(3), ps: expectedPartialSum}}]);

        // A double can always expresses 15 digits precisely. So, 1.0 + 0.00000000000001 is
        // precisely expressed by the 'addend' element.
        assert.commandWorked(coll.insert({n: 0.00000000000001}));
        expectedPartialSum = [
            NumberInt(1),           // The type id for NumberDouble
            9223372036854775808.0,  // sum
            1.00000000000001        // addend
        ];
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial sum of mixed data leading to a number that a double can't express",
            pipelineWithSum,
            [{_id: null, o: expectedPartialSum}]);
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial avg of mixed data leading to a number that a double can't express",
            pipelineWithAvg,
            [{_id: null, o: {count: NumberLong(4), ps: expectedPartialSum}}]);

        assert.commandWorked(coll.insert({n: NumberDecimal("1.0")}));
        expectedPartialSum = [
            NumberInt(1),           // The type id for NumberDouble
            9223372036854775808.0,  // sum
            1.00000000000001,       // addend
            NumberDecimal("1.0")
        ];
        verifyOverTheWireDataFormatOnBothEngines("Partial sum of mixed data which has a decimal",
                                                 pipelineWithSum,
                                                 [{_id: null, o: expectedPartialSum}]);
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial avg of mixed data which has a decimal",
            pipelineWithAvg,
            [{_id: null, o: {count: NumberLong(5), ps: expectedPartialSum}}]);

        assert(coll.drop());

        assert.commandWorked(coll.insert([{n: Number.MAX_VALUE}, {n: Number.MAX_VALUE}]));
        expectedPartialSum = [
            NumberInt(1),  // The type id for NumberDouble
            Infinity,      // sum
            NaN            // addend
        ];
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial sum of two double max", pipelineWithSum, [{_id: null, o: expectedPartialSum}]);
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial avg of two double max",
            pipelineWithAvg,
            [{_id: null, o: {count: NumberLong(2), ps: expectedPartialSum}}]);

        assert(coll.drop());

        assert.commandWorked(coll.insert([{n: NumberDecimal("1.0")}, {n: 1.0}]));
        expectedPartialSum = [
            NumberInt(1),  // The type id for NumberDouble
            1.0,           // sum
            0.0,           // addend
            NumberDecimal("1.0")
        ];
        verifyOverTheWireDataFormatOnBothEngines("Partial sum of a decimal and a double",
                                                 pipelineWithSum,
                                                 [{_id: null, o: expectedPartialSum}]);
        verifyOverTheWireDataFormatOnBothEngines(
            "Partial avg of a decimal and a double",
            pipelineWithAvg,
            [{_id: null, o: {count: NumberLong(2), ps: expectedPartialSum}}]);
    }());

    MongoRunner.stopMongod(conn);
}());

(function testShardedAccumulatorOnBothEngines() {
    const st = new ShardingTest({shards: 2});

    const db = st.getDB(jsTestName());
    const dbAtShard0 = st.shard0.getDB(jsTestName());
    const dbAtShard1 = st.shard1.getDB(jsTestName());

    // Makes sure that the test db is sharded.
    assert.commandWorked(st.s0.adminCommand({enableSharding: db.getName()}));

    let verifyShardedAccumulatorResultsOnBothEngine = (testDesc, coll, pipeline, expectedRes) => {
        // Turns to the classic engine at the shards.
        assert.commandWorked(dbAtShard0.adminCommand(
            {setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));
        assert.commandWorked(dbAtShard1.adminCommand(
            {setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}));

        // Verifies that the classic engine's results are same as the expected results.
        const classicRes = coll.aggregate(pipeline).toArray();
        assert.eq(classicRes, expectedRes, testDesc);

        // Turns to the SBE engine at the shards.
        assert.commandWorked(
            dbAtShard0.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));
        assert.commandWorked(
            dbAtShard1.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

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
        const docs = [
            {k: i, n: 1e+34},
            {k: i, n: NumberDecimal("0.1")},
            {k: i, n: NumberDecimal("0.01")},
            {k: i, n: -1e+34}
        ];
        assert.commandWorked(hashShardedColl.insert(docs));
        assert.commandWorked(unshardedColl.insert(docs));
    }

    let pipeline = [{$group: {_id: "$k", s: {$sum: "$n"}}}, {$group: {_id: "$s"}}];

    // The results on an unsharded collection is the expected results.
    let expectedRes = unshardedColl.aggregate(pipeline).toArray();
    verifyShardedAccumulatorResultsOnBothEngine(
        "Sharded sum for mixed data by which only decimal sum survive",
        hashShardedColl,
        pipeline,
        expectedRes);

    pipeline = [{$group: {_id: "$k", s: {$avg: "$n"}}}, {$group: {_id: "$s"}}];

    // The results on an unsharded collection is the expected results.
    expectedRes = unshardedColl.aggregate(pipeline).toArray();
    verifyShardedAccumulatorResultsOnBothEngine(
        "Sharded avg for mixed data by which only decimal sum survive",
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

        verifyShardedAccumulatorResultsOnBothEngine(
            testDesc, hashShardedColl, [{$group: {_id: null, o: {$sum: "$n"}}}], expectedRes);
    });

    [{testDesc: "No documents evaluated", inputs: [{}], expectedRes: [{_id: null, o: null}]},
     {
         testDesc: "One int value is converted to double",
         inputs: [{n: NumberInt(3)}],
         expectedRes: [{_id: null, o: 3.0}]
     },
     {
         testDesc: "One long value is converted to double",
         inputs: [{n: NumberLong(-4)}],
         expectedRes: [{_id: null, o: -4.0}]
     },
     {testDesc: "One double value", inputs: [{n: 22.6}], expectedRes: [{_id: null, o: 22.6}]},
     {
         testDesc: "Averaging two ints",
         inputs: [{n: NumberInt(10)}, {n: NumberInt(11)}],
         expectedRes: [{_id: null, o: 10.5}]
     },
     {
         testDesc: "Averaging two longs",
         inputs: [{n: NumberLong(10)}, {n: NumberLong(11)}],
         expectedRes: [{_id: null, o: 10.5}]
     },
     {
         testDesc: "Averaging two doubles",
         inputs: [{n: 10.0}, {n: 11.0}],
         expectedRes: [{_id: null, o: 10.5}]
     },
     {
         testDesc: "The average of an int and a double is a double",
         inputs: [{n: NumberInt(10)}, {n: 11.0}],
         expectedRes: [{_id: null, o: 10.5}]
     },
     {
         testDesc: "The average of a long and a double is a double",
         inputs: [{n: NumberLong(10)}, {n: 11.0}],
         expectedRes: [{_id: null, o: 10.5}]
     },
     {
         testDesc: "The average of an int and a long is a double",
         inputs: [{n: NumberInt(5)}, {n: NumberLong(3)}],
         expectedRes: [{_id: null, o: 4.0}]
     },
     {
         testDesc: "Averaging an int, long, and double",
         inputs: [{n: NumberInt(1)}, {n: NumberLong(2)}, {n: 6.0}],
         expectedRes: [{_id: null, o: 3.0}]
     },
     {
         testDesc: "Unlike $sum, two ints do not overflow in the 'total' portion of the average",
         inputs: [{n: numberIntMax}, {n: numberIntMax}],
         expectedRes: [{_id: null, o: int32Max}]
     },
     {
         testDesc: "Two longs do overflow in the 'total' portion of the average",
         inputs: [{n: numberLongMax}, {n: numberLongMax}],
         expectedRes: [{_id: null, o: doubleClosestToLongMax}]
     },
     {
         testDesc: "Averaging an Infinity and a number",
         inputs: [{n: Infinity}, {n: 1}],
         expectedRes: [{_id: null, o: Infinity}]
     },
     {
         testDesc: "Averaging two Infinities",
         inputs: [{n: Infinity}, {n: Infinity}],
         expectedRes: [{_id: null, o: Infinity}]
     },
     {
         testDesc: "Averaging an Infinity and an NaN",
         inputs: [{n: Infinity}, {n: NaN}],
         expectedRes: [{_id: null, o: NaN}]
     },
     {
         testDesc: "Averaging an NaN and a number",
         inputs: [{n: NaN}, {n: 1}],
         expectedRes: [{_id: null, o: NaN}]
     },
     {
         testDesc: "Averaging two NaNs",
         inputs: [{n: NaN}, {n: NaN}],
         expectedRes: [{_id: null, o: NaN}]
     },
     {
         testDesc: "Averaging two decimals",
         inputs: [
             {n: NumberDecimal("-1234567890.1234567889")},
             {n: NumberDecimal("-1234567890.1234567891")}
         ],
         expectedRes: [{_id: null, o: NumberDecimal("-1234567890.1234567890")}]
     },
     {
         testDesc: "Averaging two longs and a decimal results in an accurate decimal result",
         inputs: [
             {n: NumberLong("1234567890123456788")},
             {n: NumberLong("1234567890123456789")},
             {n: NumberDecimal("1234567890123456790.037037036703702")}
         ],
         expectedRes: [{_id: null, o: NumberDecimal("1234567890123456789.012345678901234")}]
     },
     {
         testDesc: "Averaging a double and a decimal",
         inputs: [{n: 1.0e22}, {n: NumberDecimal("9999999999999999999999.9999999999")}],
         expectedRes: [{_id: null, o: NumberDecimal("9999999999999999999999.99999999995")}]
     },
    ].forEach(({testDesc, inputs, expectedRes}) => {
        hashShardedColl.drop();
        assert.commandWorked(hashShardedColl.insert(inputs));

        verifyShardedAccumulatorResultsOnBothEngine(
            testDesc, hashShardedColl, [{$group: {_id: null, o: {$avg: "$n"}}}], expectedRes);
    });

    st.stop();
}());
}());
