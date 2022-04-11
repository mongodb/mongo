// Tests expression object compilation with different placements of expression objects in the
// expression tree contained in the projection.

(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

const coll = db.expression_object;
coll.drop();
assert.commandWorked(coll.insert({'_id': 'doc1', 'field': 'val1'}));

// All of the following tests filter nothing out and only compute the required projections.
// Each test contains Expression Object(s) at different positions within the expression tree.
// The fourth test projects the comparison result of two empty objects, which is true.
const emptyFilterDoc = {};
const testCases = [
    {projectionDoc: {label1: '$fieldRef1'}, resultDoc: {_id: 'doc1'}},
    {
        projectionDoc: {label2: {'$eq': ['$_id', {label21: '$fieldRef21'}]}},
        resultDoc: {_id: 'doc1', label2: false}
    },
    {
        projectionDoc: {label3: {'$eq': [{label31: '$fieldRef31'}, '$_id']}},
        resultDoc: {_id: 'doc1', label3: false}
    },
    {
        projectionDoc: {label4: {'$eq': [{label41: '$fieldRef41'}, {label42: '$fieldRef42'}]}},
        resultDoc: {_id: 'doc1', label4: true}
    },
    {
        projectionDoc: {
            label5: {
                '$eq': [
                    {label51: {'$eq': ['$_id', {label511: '$fieldRef511'}]}},
                    {label52: {'$eq': [{label521: '$fieldRef521'}, '$_id']}}
                ]
            }
        },
        resultDoc: {_id: 'doc1', label5: false}
    },
    {
        projectionDoc:
            {label6: {'$eq': [{label61: {'$eq': [{label611: '$fieldRef611'}, '$_id']}}, '$_id']}},
        resultDoc: {_id: 'doc1', label6: false}
    }
];

for (let testCase of testCases) {
    assert(documentEq(testCase.resultDoc, coll.findOne(emptyFilterDoc, testCase.projectionDoc)));
}
}());
