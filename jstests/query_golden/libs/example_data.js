
// Helpers for generating small but diverse data examples.

// Generates interesting "leaf" values: values that don't contain other values.
// This includes [] and {}.
function leafs() {
    // See bsontypes.h or https://bsonspec.org/ for a complete list of BSON types.
    // Not every type is represented here.
    return [
        MinKey,

        // 'missing', aka "$$REMOVE" is not included here.
        // It's not a first-class value because it can't be an element of an array.
        // Instead, we can generate objects where a given field is missing.

        // NumberDouble
        NaN,
        -Infinity,
        -1.79769313486231570815e+308,
        -2.0,
        -1.0,
        -5e-324,
        -0.0,
        0.0,
        5e-324,
        1.0,
        2.0,
        -1.79769313486231570815e+308,
        +Infinity,

        // String
        "",
        "a",
        "A",
        "\u{1f600}",  // A code point that doesn't fit in a 16-bit unit. (A smiley emoji.)

        // Object
        {},

        // Array
        [],

        // BinData
        BinData(0, ''),
        BinData(0, 'asdf'),
        // UUID is one subtype of BinData.
        UUID("326d92af-2d76-452b-a03f-69f05ab98416"),
        UUID("167c25c0-4f45-488a-960a-3171ec07726b"),

        undefined,

        // ObjectId
        // See oid.h, or https://www.mongodb.com/docs/manual/reference/method/ObjectId/.
        ObjectId("62d05ec744ca83616c92772c"),
        ObjectId("62d05fa144ca83616c92772e"),
        ObjectId('000000000000000000000000'),
        ObjectId('ffffffffffffffffffffffff'),

        // Boolean
        false,
        true,

        // Date
        ISODate("2022-07-14T18:34:28.937Z"),
        ISODate("0000-01-01T00:00:00Z"),      // The smallest formattable date.
        ISODate("9999-12-31T23:59:59.999Z"),  // The greatest formattable date.
        new Date(-1),                         // The greatest negative date, 1ms before 1970.
        new Date(0),                          // The zero date, at 1970.

        null,

        // RegEx
        new RegExp(''),
        /a/,
        /A/,
        /a/i,

        // Code
        function inc(x) {
            return x + 1;
        },

        // Symbol--deprecated and maybe not even representable in Javascript.

        // NumberInt
        NumberInt('-2147483648'),
        NumberInt(-2),
        NumberInt(-1),
        NumberInt(0),
        NumberInt(1),
        NumberInt(2),
        NumberInt('+2147483647'),

        NumberLong('-9223372036854775808'),
        NumberLong(-2),
        NumberLong(-1),
        NumberLong(0),
        NumberLong(1),
        NumberLong(2),
        NumberLong('+9223372036854775807'),

        NumberDecimal('NaN'),
        NumberDecimal('-Infinity'),
        NumberDecimal('-9.999999999999999999999999999999999e6144'),
        NumberDecimal('-1.000'),
        NumberDecimal('-1'),
        NumberDecimal('-1e-6176'),
        NumberDecimal('-0.000'),
        NumberDecimal('-0'),
        NumberDecimal('0'),
        NumberDecimal('0.000'),
        NumberDecimal('1e-6176'),
        NumberDecimal('1'),
        NumberDecimal('9.999999999999999999999999999999999e6144'),
        NumberDecimal('-Infinity'),

        MaxKey,
    ];
}

// Documents with (at most) a single field with the given name.
// Includes the "missing value" by including one empty doc.
function unaryDocs(fieldname, values) {
    return values.map(v => ({[fieldname]: v}));
}

// Arrays with exactly one element.
function unaryArrays(values) {
    return values.map(v => [v]);
}

function smallDocs() {
    let values = leafs();
    values = values.concat(unaryDocs('x', values)).concat(unaryArrays(values));
    return unaryDocs('a', values);
}

// Prepend an '_id' field to each document, numbered sequentially from 0.
// Preserves any existing '_id' value, but always moves that field to the beginning.
function sequentialIds(docs) {
    let i = 0;
    return docs.map(d => Object.merge({_id: i++}, d));
}
