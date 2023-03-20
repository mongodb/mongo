/**
 * Tests that count on non-wildcard field returns correct results when a compound wildcard index is
 * used.
 *
 * @tags: [
 *   not_allowed_with_security_token,
 *   does_not_support_stepdowns,
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */

(function() {
"use strict";

function testCount(coll, indexKeyPattern, doc, filter) {
    coll.drop();
    coll.createIndex(indexKeyPattern);
    coll.insertOne(doc);
    const findCount = coll.find(filter).count();
    assert.eq(1, findCount);
    const aggregateCount = coll.aggregate([{$match: filter}, {$count: "count"}]).toArray();
    assert.eq(1, aggregateCount[0].count);
}

const docs = [
    {
        _id: 269,
        obj: {
            obj: {
                date: ISODate("2019-06-24T22:40:06.624Z"),
                obj: {
                    _id: 273,
                    str: 'Zimbabwe Svalbard & Jan Mayen Islands Personal Loan Account',
                    date: null,
                },
            }
        }
    },
    {
        _id: 269,
        obj: {
            obj: {
                date: ISODate("2019-06-24T22:40:06.624Z"),
                obj: {
                    _id: 273,
                    str: 'Zimbabwe Svalbard & Jan Mayen Islands Personal Loan Account',
                    date: null,
                    array: null,
                    obj: {
                        _id: 274,
                        str: 'partnerships',
                        date: ISODate("2019-07-24T16:08:45.196Z"),
                        array: [],
                        geoJson: {type: 'Point', coordinates: [Array]},
                        any: 'virtual intangible Chair'
                    }
                }
            },
        }
    },
];

const indexKeyPattern = {
    'obj.obj.date': -1,
    'obj.obj.obj.$**': -1
};

const filter = {
    'obj.obj.date': {'$lte': ISODate("2019-10-06T22:12:37.493Z")}
};

for (const doc of docs) {
    testCount(db.testColl, indexKeyPattern, doc, filter);
}
}());
