(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For assertArrayEq

const t = db.type_bracket;
t.drop();
const docs = [
    {_id: 0, a: MinKey()},
    {_id: 1, a: null},
    {_id: 2, a: NumberLong(1)},
    {_id: 3, a: NumberDecimal("2.0")},
    {_id: 4, a: 3},
    {_id: 5, a: ""},
    {_id: 6, a: "hello"},
    {_id: 7, a: Number.MAX_VALUE},
    {_id: 8, a: new Date("1969-08-20T00:00:00")},
    {_id: 9, a: new Date("1970-08-20T00:00:00")},
    {_id: 10, a: new Timestamp(1, 1)},
    {_id: 11, a: new Timestamp(2, 1)},
    {_id: 12, a: {c: "hello"}},
    {_id: 13, a: {}},
    {_id: 14, a: []},
    {_id: 15, a: [1, 2, 3]},
    {_id: 16, a: [{c: "1"}, {d: "2"}]},
    {_id: 17, a: new BinData(0, '1234')},
    {_id: 18, a: new BinData(0, 'aaaa')},
    {_id: 19, a: new BinData(0, '')},
    {_id: 20, a: {$regularExpression: {pattern: "a", options: ""}}},
    {_id: 21, a: /a+/},
    {_id: 22, a: new DBRef("type_bracket", 15)},
    {_id: 23, a: new ObjectId('000000000000000000000000')},
    {_id: 24, a: new Code('x = 1+1;', {})},
    {_id: 25, a: new Code('', {})},
    {_id: 26, a: true},
    {_id: 27, a: false},
    {_id: 28, a: new Code("noScope")},
    {_id: 29, b: ""},
    {_id: 30, a: 0},
    {_id: 0xffffffffffffffffffffffff, a: MaxKey()},
];

assert.commandWorked(t.insert(docs));

const runTest = (filter, expected) => {
    try {
        // Disable pipeline optimization because it is unclear if pipeline optimization is masking
        // some bugs in ABT translation
        assert.commandWorked(db.adminCommand(
            {'configureFailPoint': 'disablePipelineOptimization', 'mode': 'alwaysOn'}));

        const result = t.aggregate({$match: filter}).toArray();
        assertArrayEq({
            actual: result,
            expected: expected,
        });
    } finally {
        assert.commandWorked(
            db.adminCommand({'configureFailPoint': 'disablePipelineOptimization', 'mode': 'off'}));
    }
};
const tests = [
    // Number
    {filter: {a: {$gt: NumberInt(0)}}, expected: [docs[2], docs[3], docs[4], docs[7], docs[15]]},
    {filter: {a: {$lte: 0}}, expected: [docs[30]]},

    // String
    {filter: {a: {$gt: "h"}}, expected: [docs[6]]},
    {filter: {a: {$lte: "h"}}, expected: [docs[5]]},

    // Object
    {filter: {a: {$gte: {}}}, expected: [docs[12], docs[13], docs[16], docs[20], docs[22]]},
    {filter: {a: {$lte: {}}}, expected: [docs[13]]},

    // Array
    {filter: {a: {$gt: [1]}}, expected: [docs[15], docs[16]]},
    {filter: {a: {$gt: [{}]}}, expected: [docs[16]]},
    {filter: {a: {$lte: [1]}}, expected: [docs[14]]},

    // BinData
    {filter: {a: {$gte: new BinData(0, "1234")}}, expected: [docs[17]]},
    {filter: {a: {$lt: new BinData(0, "1234")}}, expected: [docs[18], docs[19]]},

    // ObjectID
    {filter: {a: {$gt: new ObjectId()}}, expected: []},
    {filter: {a: {$lte: new ObjectId()}}, expected: [docs[23]]},

    // Date
    {filter: {a: {$lt: new Date('2019-09-18')}}, expected: [docs[8], docs[9]]},
    {filter: {a: {$gte: new Date('2019-01-01')}}, expected: []},

    // Timestamp
    {filter: {a: {$lte: new Timestamp(3, 1)}}, expected: [docs[10], docs[11]]},
    {filter: {a: {$gte: new Timestamp(10, 1)}}, expected: []},

    // Null
    // TODO SERVER-67853 Uncomment after fixing ABT Translation for comparing GTE and LTE null
    // {filter: {a: {$gte: null}}, expected: [docs[1], docs[29]]},
    // {filter: {a: {$lte: null}}, expected: [docs[1], docs[29]]},

    {filter: {a: {$gte: false}}, expected: [docs[26], docs[27]]},
    {filter: {a: {$lt: false}}, expected: []},

    // Comparison with Regex strings is invalid

    // DBRef
    {
        filter: {a: {$gte: new DBRef("type_bracket", 15)}},
        expected: [docs[12], docs[16], docs[20], docs[22]]
    },
    {filter: {a: {$lt: new DBRef("type_bracket", 15)}}, expected: [docs[13]]},
    {
        filter: {a: {$gte: new DBRef("type_bracket", 14)}},
        expected: [docs[12], docs[16], docs[20], docs[22]]
    },

    // MaxKey
    // TODO SERVER-67624 Uncomment after fixing translations for match exprs against MinKey/MaxKey
    // TODO SERVER-67853 Uncomment after fixing ABT Translation for comparing GTE and LTE min/maxkey
    // {filter: {a: {$lte: MaxKey()}}, expected: docs},
    // {filter: {a: {$gte: MaxKey()}}, expected: docs[docs.length - 1]},

    // CODEWSCOPE
    {filter: {a: {$gte: new Code('function() {x++;}', {})}}, expected: [docs[24]]},
    {filter: {a: {$lt: new Code('x', {})}}, expected: [docs[25]]},

    // CODE
    {filter: {a: {$gte: new Code("")}}, expected: [docs[28]]},
    {filter: {a: {$lte: new Code("")}}, expected: []},

];

for (const testData of tests) {
    runTest(testData.filter, testData.expected);
}
}());