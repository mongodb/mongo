
/**
 * Test that a $function behaves as expected when its return value is a BSON scalar, i.e. a value
 * that is not an object or array.
 *
 * @tags: [requires_fcv_60]
 */

(function() {
'use strict';

const coll = db.return_bson_scalare_from_js_function;

coll.drop();
coll.insert({name: "Boolean", expected: "Boolean", x: true});
coll.insert({name: "String", expected: "String", x: "foo"});
coll.insert({name: "Number", expected: "Number", x: 1});
coll.insert({name: "NumberLong", expected: "NumberLong", x: NumberLong("1")});
coll.insert({name: "NumberDecimal", expected: "NumberDecimal", x: NumberDecimal("1")});
coll.insert({name: "ObjectId", expected: "ObjectId", x: ObjectId()});
coll.insert({name: "BinData", expected: "BinData", x: BinData(0, "")});
coll.insert({name: "Timestamp", expected: "Timestamp", x: Timestamp(1, 1)});
coll.insert({name: "MinKey", expected: "MinKey", x: MinKey()});
coll.insert({name: "MaxKey", expected: "MaxKey", x: MaxKey()});
coll.insert({name: "RegExp", expected: "RegExp", x: /foo/g});
coll.insert({name: "RegExp-with-slash", expected: "RegExp", x: /\/foo/g});
coll.insert({name: "Date", expected: "Date", x: new Date()});
coll.insert({name: "Code", expected: "Code", x: Code("function() { return 1; }")});
// Note that Symbol is not supported: SERVER-63709, SERVER-63711.

// Although 'x' has "NumberInt" type in the collection, it behaves as a native Javascript (double)
// number in this test. The ValueWriter that translates the $function return value converts all
// NumberInt values to native numbers so they are safe to use in comparisons. See SERVER-5424.
coll.insert({name: "NumberInt", expected: "Number", x: NumberInt("1")});

const pipeline = [{
    $addFields: {
        nested: {
            $function: {
                body: function(x) {
                    return {x: x};
                },
                args: ["$x"],
                lang: "js",

            }
        },
        toplevel: {
            $function: {
                body: function(x) {
                    return x;
                },
                args: ["$x"],
                lang: "js",

            }
        },
    }
}];

coll.aggregate(pipeline).forEach(doc => {
    assert.eq(doc.expected, doc.nested.x.constructor.name, doc);
    assert.eq(doc.expected, doc.toplevel.constructor.name, doc);
});
}());
