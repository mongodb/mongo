/**
 * Tests that '$_internalIndexKey' expression works as expected under various scenarios.
 *
 * @tags: [requires_fcv_63]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");  // For "FeatureFlagUtil"

const collection = db.index_key_expression;

/**
 * Returns the hash of the provided BSON element that is compatible with 'hashed' indexes.
 */
function getHash(bsonElement) {
    return assert.commandWorked(db.runCommand({_hashBSONElement: bsonElement, seed: 0})).out;
}

// A dictionary consisting of various test scenarios that must be run against the
// '$_internalIndexKey'.
//
// Below is the description about the fields:
// doc - The document that must be inserted to the collection. The '$_internalIndexKey' would
//       observe this document using '$$ROOT' and generate the corresponding keys if possible.
// spec - The index specification document that is passed to the '$_internalIndexKey'.
// expectedIndexKeys - The keys document that the '$_internalIndexKey' must returned corresponding
//                     to the 'doc' and the 'spec'.
// expectedErrorCode - The error code that the '$_internalIndexKey' must throw in case of an
//                     exception.
const testScenarios = [
    //
    // Test '$_internalIndexKey' against invalid arguments.
    //
    {
        doc: {a: 4, b: 5},
        expectedErrorCode: 6868509  // $_internalIndexKey requires both 'doc' and 'spec' arguments.
    },
    {
        spec: {key: {a: 1}, name: "btreeIndex"},
        expectedErrorCode: 6868509  // $_internalIndexKey requires both 'doc' and 'spec' arguments.
    },
    {
        doc: null,
        spec: {key: {a: 1}, name: "btreeIndex"},
        expectedErrorCode: 6868509  // $_internalIndexKey requires both 'doc' and 'spec' arguments.
    },
    {
        doc: {a: 4, b: 5},
        spec: null,
        expectedErrorCode: 6868509  // $_internalIndexKey requires both 'doc' and 'spec' arguments.
    },
    {
        doc: {a: 4, b: 5},
        spec: {key: {b: 1, a: -1}, name: "btreeIndex"},
        expectedErrorCode: 6868501  // Index key pattern field ordering must be ascending.
    },
    {
        doc: {a: 4, b: 5},
        spec: "spec",
        expectedErrorCode: 6868507  // $_internalIndexKey requires 'spec' argument to be an object.
    },
    {
        doc: {a: 4, b: 5},
        spec: {a: {$const: 1}},
        expectedErrorCode:
            ErrorCodes.InvalidIndexSpecificationOption  // The field 'a' is not valid
                                                        // for an index specification.
    },
    {
        doc: {a: 4, b: 5},
        spec: {a: {$literal: 1}},
        expectedErrorCode:
            ErrorCodes.InvalidIndexSpecificationOption  // The field 'a' is not valid for an index
                                                        // specification.
    },
    {
        doc: {a: 4, b: 5},
        spec: {$literal: {key: {a: 1}, name: "btreeIndex"}},
        expectedErrorCode:
            ErrorCodes.InvalidIndexSpecificationOption  // The field '$literal' is not valid for an
                                                        // index specification.
    },
    {
        doc: {a: 4, b: 5},
        spec: {$const: {key: {a: 1}, name: "btreeIndex"}},
        expectedErrorCode:
            ErrorCodes.InvalidIndexSpecificationOption  // The field '$const' is not valid for an
                                                        // index specification.
    },

    //
    // ++++ Btree index tests ++++
    // Test '$_internalIndexKey' against various shapes of 'doc' and 'spec'. The 'doc' contains
    // nested fields, arrays and combination of both. The 'spec' contains simple/composite fields,
    // dotted field paths and combination of both.
    //
    {doc: {a: 4, b: 5}, spec: {key: {a: 1}, name: "btreeIndex"}, expectedIndexKeys: [{a: 4}]},
    {
        doc: {a: 4, b: 5},
        spec: {key: {a: 1, b: 1}, name: "btreeIndex"},
        expectedIndexKeys: [{a: 4, b: 5}]
    },
    {
        doc: {a: 4, b: 5},
        spec: {key: {b: 1, a: 1}, name: "btreeIndex"},
        expectedIndexKeys: [{b: 5, a: 4}]
    },
    {
        doc: {a: {b: 4}, c: "c"},
        spec: {key: {"a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": 4}]
    },
    {
        doc: {a: [1, 2, 3]},
        spec: {key: {a: 1}, name: "btreeIndex"},
        expectedIndexKeys: [{a: 1}, {a: 2}, {a: 3}]
    },
    {
        doc: {a: [1, 2, 3], b: 2},
        spec: {key: {a: 1, b: 1}, name: "btreeIndex"},
        expectedIndexKeys: [{a: 1, b: 2}, {a: 2, b: 2}, {a: 3, b: 2}]
    },
    {
        doc: {a: 5, b: [1, 2, 3]},
        spec: {key: {b: 1, a: 1}, name: "btreeIndex"},
        expectedIndexKeys: [{b: 1, a: 5}, {b: 2, a: 5}, {b: 3, a: 5}]
    },
    {doc: {a: [0, 0, 0]}, spec: {key: {a: 1}, name: "btreeIndex"}, expectedIndexKeys: [{a: 0}]},
    {
        doc: {a: {b: [1, 2, 3]}},
        spec: {key: {"a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": 1}, {"a.b": 2}, {"a.b": 3}]
    },
    {
        doc: {a: [{b: 1, c: 4}, {b: 2, c: 4}, {b: 3, c: 4}]},
        spec: {key: {"a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": 1}, {"a.b": 2}, {"a.b": 3}]
    },
    {
        doc: {a: {b: [{c: 1}, {c: 2}]}},
        spec: {key: {"a.b.c": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b.c": 1}, {"a.b.c": 2}]
    },
    {
        doc: {a: [{b: 1, c: 4, e: 6}, {e: 6, b: 2, c: 4}, {b: 3, e: 6, c: 4}], d: 5},
        spec: {key: {"a.b": 1, d: 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": 1, d: 5}, {"a.b": 2, d: 5}, {"a.b": 3, d: 5}]
    },
    {
        doc: {a: [{b: 1}, {b: [1, 2, 3]}]},
        spec: {key: {"a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": 1}, {"a.b": 2}, {"a.b": 3}]
    },
    {
        doc: {a: [{b: [1, 2]}, {b: [2, 3]}]},
        spec: {key: {"a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": 1}, {"a.b": 2}, {"a.b": 3}]
    },
    {
        doc: {a: [{b: [1, 2, 3]}, {b: [2]}, {b: [3, 1]}]},
        spec: {key: {"a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": 1}, {"a.b": 2}, {"a.b": 3}]
    },
    {
        doc: {a: [{b: 2}]},
        spec: {key: {"a": 1, "a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{a: {b: 2}, "a.b": 2}]
    },
    {doc: {a: [2]}, spec: {key: {"a.0": 1}, name: "btreeIndex"}, expectedIndexKeys: [{"a.0": 2}]},
    {
        doc: {a: [[2]]},
        spec: {key: {"a.0": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.0": [2]}]
    },
    {
        doc: {a: {"0": 2}},
        spec: {key: {"a.0": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.0": 2}]
    },
    {
        doc: {a: [[2]], c: 3},
        spec: {key: {"a.0.0": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.0.0": 2}]
    },
    {
        doc: {a: {b: {c: [0, 2, 3, [4]]}}},
        spec: {key: {"a.b.c.3": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b.c.3": [4]}]
    },
    {
        doc: {a: [{b: [1, 2]}, {b: {0: 3}}]},
        spec: {key: {a: 1, "a.b": 1, "a.0.b": 1, "a.b.0": 1, "a.0.b.0": 1}, name: "btreeIndex"},
        expectedIndexKeys: [
            {a: {b: {0: 3}}, "a.b": {0: 3}, "a.0.b": 1, "a.b.0": 3, "a.0.b.0": 1},
            {a: {b: {0: 3}}, "a.b": {0: 3}, "a.0.b": 2, "a.b.0": 3, "a.0.b.0": 1},
            {a: {b: [1, 2]}, "a.b": 1, "a.0.b": 1, "a.b.0": 1, "a.0.b.0": 1},
            {a: {b: [1, 2]}, "a.b": 2, "a.0.b": 2, "a.b.0": 1, "a.0.b.0": 1}
        ]
    },

    //
    // ++++ Btree index tests ++++
    // These cases cause the '$_internalIndexKey' to return null and undefined keys for some or all
    // fields.
    //
    {
        doc: {a: [{c: 2}, {c: 2}, {c: 2}]},
        spec: {key: {"a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": null}]
    },
    {doc: {b: 1}, spec: {key: {a: 1}, name: "btreeIndex"}, expectedIndexKeys: [{a: null}]},
    {
        doc: {a: [1, 2]},
        spec: {key: {"a.b": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.b": null}]
    },
    {
        doc: {a: "a"},
        spec: {key: {x: 1, y: 1}, name: "btreeIndex"},
        expectedIndexKeys: [{x: null, y: null}]
    },
    {doc: {a: []}, spec: {key: {a: 1}, name: "btreeIndex"}, expectedIndexKeys: [{a: undefined}]},
    {doc: {a: null}, spec: {key: {a: 1}, name: "btreeIndex"}, expectedIndexKeys: [{a: null}]},
    {
        doc: {a: [[]]},
        spec: {key: {"a.0.0": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.0.0": null}]
    },
    {
        doc: {a: []},
        spec: {key: {"a.0.0": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.0.0": null}]
    },
    {
        doc: {a: [[]]},
        spec: {key: {"a.0": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.0": undefined}]
    },
    {
        doc: {a: [[1, [1, 2, [{b: [[], 2]}]]], 1]},
        spec: {key: {"a.0.1.2.b.0": 1}, name: "btreeIndex"},
        expectedIndexKeys: [{"a.0.1.2.b.0": undefined}]
    },
    {
        doc: {a: 2, b: []},
        spec: {key: {a: 1, b: 1}, name: "btreeIndex"},
        expectedIndexKeys: [{a: 2, b: undefined}]
    },
    {
        doc: {a: [1, {b: [2, {c: [3, {d: 1}], e: 4}, 5, {f: 6}], g: 7}]},
        spec:
            {key: {"a.b.c.d": 1, "a.g": 1, "a.b.f": 1, "a.b.c": 1, "a.b.e": 1}, name: "btreeIndex"},
        expectedIndexKeys: [
            {"a.b.c.d": null, "a.g": null, "a.b.f": null, "a.b.c": null, "a.b.e": null},
            {"a.b.c.d": null, "a.g": 7, "a.b.f": null, "a.b.c": null, "a.b.e": null},
            {"a.b.c.d": null, "a.g": 7, "a.b.f": null, "a.b.c": 3, "a.b.e": 4},
            {"a.b.c.d": null, "a.g": 7, "a.b.f": 6, "a.b.c": null, "a.b.e": null},
            {"a.b.c.d": 1, "a.g": 7, "a.b.f": null, "a.b.c": {"d": 1}, "a.b.e": 4}
        ]
    },

    //
    // ++++ Btree index tests ++++
    // Test '$_internalIndexKey' by passing collation.
    //
    {
        doc: {a: "2", b: 3},
        spec: {key: {a: 1}, name: "btreeIndex", collation: {locale: "en"}},
        expectedIndexKeys: [{a: "\u0016\u0001\u0005\u0001\u0005"}]
    },
    {
        doc: {a: {b: "2"}, c: 3},
        spec: {key: {"a.b": 1}, name: "btreeIndex", collation: {locale: "en"}},
        expectedIndexKeys: [{"a.b": "\u0016\u0001\u0005\u0001\u0005"}]
    },
    {
        doc: {a: ["2", "3", "4"]},
        spec: {key: {a: 1}, name: "btreeIndex", collation: {locale: "en"}},
        expectedIndexKeys: [
            {"a": "\u0016\u0001\u0005\u0001\u0005"},
            {"a": "\u0018\u0001\u0005\u0001\u0005"},
            {"a": "\u001a\u0001\u0005\u0001\u0005"}
        ]
    },
    {
        doc: {b: 2, a: {c: "3"}},
        spec: {key: {a: 1}, name: "btreeIndex", collation: {locale: "en"}},
        expectedIndexKeys: [{"a": {c: "\u0018\u0001\u0005\u0001\u0005"}}]
    },

    //
    // ++++ Btree index tests ++++
    // These tests causes '$_internalIndexKey' to throw exceptions for btree indexes.
    //
    {
        doc: {a: [{"0": 2}]},
        spec: {key: {"a.0": 1}, name: "btreeIndex"},
        expectedErrorCode: 16746  // Ambiguous field name found in array.
    },
    {
        doc: {a: [2, {"0": 3}]},
        spec: {key: {"a.0": 1}, name: "btreeIndex"},
        expectedErrorCode: 16746  // Ambiguous field name found in array.
    },
    {
        doc: {a: [1, 2, 3], b: [1, 2, 3]},
        spec: {key: {"a": 1, "b": 1}, name: "btreeIndex"},
        expectedErrorCode:
            ErrorCodes.CannotIndexParallelArrays  // Cannot index parallel arrays [b] [a].
    },
    {
        doc: {a: "2", b: 3},
        spec: {key: {a: 1}, name: "btreeIndex", collation: {backwards: true}},
        expectedErrorCode: 6868502  // Malformed 'collation' document provided.
    },
    {
        doc: {a: 2},
        spec: {key: {".a": 1}, name: "btreeIndex"},
        expectedErrorCode:
            ErrorCodes.CannotCreateIndex  // Index keys cannot contain an empty field.
    },
    {
        doc: {a: {"": [{c: 1}, {c: 2}]}},
        spec: {key: {"a..c": 1}, name: "btreeIndex"},
        expectedErrorCode:
            ErrorCodes.CannotCreateIndex  // Index keys cannot contain an empty field.
    },
    {
        doc: {a: 2},
        spec: {key: {"a.": 1}, name: "btreeIndex"},
        expectedErrorCode:
            ErrorCodes.CannotCreateIndex  // Index keys cannot contain an empty field.
    },
    {
        doc: {a: {"": 2}},
        spec: {key: {"a.": 1}, name: "btreeIndex"},
        expectedErrorCode:
            ErrorCodes.CannotCreateIndex  // Index keys cannot contain an empty field.
    },

    //
    // ++++ Hashed index tests ++++
    // Test '$_internalIndexKey' against various shapes of 'doc' and 'spec' with hashed index.
    //
    {
        doc: {a: "a"},
        spec: {key: {a: "hashed"}, name: "hashedIndex"},
        expectedIndexKeys: [{a: getHash("a")}]
    },
    {
        doc: {a: {b: 2}, c: 3},
        spec: {key: {a: "hashed"}, name: "hashedIndex", collation: {locale: "simple"}},
        expectedIndexKeys: [{a: getHash({b: 2})}]
    },
    {
        doc: {a: 2, c: "c", b: {c: 100}},
        spec: {key: {a: "hashed", b: 1}, name: "hashedIndex"},
        expectedIndexKeys: [{a: getHash(2), b: {c: 100}}]
    },
    {
        doc: {a: {b: "abc", c: "def"}},
        spec: {key: {"a.c": 1, a: "hashed"}, name: "hashedIndex"},
        expectedIndexKeys: [{"a.c": "def", a: getHash({b: "abc", c: "def"})}]
    },
    {
        doc: {a: {b: {c: "abc", d: {e: "def"}}}, f: "ghi"},
        spec: {key: {"a.b.d.e": "hashed", f: 1}, name: "hashedIndex"},
        expectedIndexKeys: [{"a.b.d.e": getHash("def"), f: "ghi"}]
    },
    {
        doc: {a: "a"},
        spec: {key: {a: "hashed"}, name: "hashedIndex", collation: {locale: "simple"}},
        expectedIndexKeys: [{a: getHash("a")}]
    },
    {
        doc: {a: "a"},
        spec: {key: {a: "hashed"}, name: "hashedIndex", collation: {locale: "en"}},
        expectedIndexKeys: [{a: NumberLong("537359449531599826")}]
    },
    {
        doc: {},
        spec: {key: {a: "hashed", b: 1, c: 1}, name: "hashedIndex"},
        expectedIndexKeys: [{a: getHash(null), b: null, c: null}]
    },
    {
        doc: {a: 2},
        spec: {key: {b: "hashed", c: 1}, name: "hashedIndex"},
        expectedIndexKeys: [{b: getHash(null), c: null}]
    },
    {
        doc: {a: 2},
        spec: {key: {b: "hashed", c: 1}, name: "hashedIndex", collation: {locale: "en"}},
        expectedIndexKeys: [{b: getHash(null), c: null}]
    },
    {
        doc: {a: "a", b: 2},
        spec: {key: {a: "hashed", b: "hashed"}, name: "hashedIndex", collation: {locale: "simple"}},
        expectedErrorCode:
            31303  // A maximum of one index field is allowed to be hashed but found 2.
    },
    {
        doc: {a: [{b: 2}], c: 3},
        spec: {key: {a: "hashed"}, name: "hashedIndex", collation: {locale: "simple"}},
        expectedErrorCode: 16766  // hashed indexes do not currently support array values.
    },

    //
    // ++++ 2d index tests ++++
    // Test '$_internalIndexKey' against various shapes of 'doc' and 'spec' with 2d index.
    //
    {
        doc: {a: [0, 0]},
        spec: {key: {a: "2d"}, name: "2DIndex"},
        expectedIndexKeys: [{a: BinData(128, "wAAAAAAAAAA=")}]
    },
    {
        doc: {a: [0, 0], b: 5, e: 100, c: 200},
        spec: {key: {a: "2d", c: 1, b: 1}, name: "2DIndex"},
        expectedIndexKeys: [{a: BinData(128, "wAAAAAAAAAA="), c: 200, b: 5}]
    },
    {
        doc: {c: 100, d: 400, a: {e: [0, 0]}, b: 5},
        spec: {key: {"a.e": "2d", d: 1, b: 1}, name: "2DIndex"},
        expectedIndexKeys: [{"a.e": BinData(128, "wAAAAAAAAAA="), d: 400, b: 5}]
    },
    {
        doc: {a: [0, 0], b: [5, 6]},
        spec: {key: {a: "2d", b: 1}, name: "2DIndex"},
        expectedIndexKeys: [{a: BinData(128, "wAAAAAAAAAA="), b: [5, 6]}]
    },
    {
        doc: {a: [0, 0], b: 5, e: 100, c: 200},
        spec: {key: {f: "2d", g: 1, h: 1}, name: "2DIndex"},
        expectedIndexKeys: []
    },
    {
        doc: {a: [0, 0], b: [5, 6]},
        spec: {key: {a: "2d", b: "2d"}, name: "2DIndex"},
        expectedErrorCode: 16800  // can't have 2 geo fields.
    },
    {
        doc: {a: [0], b: 5},
        spec: {key: {a: "2d", b: 1}, name: "2DIndex"},
        expectedErrorCode: 13068  // geo field only has 1 element.
    },

    //
    // ++++ 2dsphere index tests ++++
    // Test '$_internalIndexKey' against various shapes of 'doc' and 'spec' with 2dsphere index.
    //
    {
        doc: {
            a: {
                b: [
                    {nongeo: 1, geo: {type: "Point", coordinates: [0, 0]}},
                    {nongeo: 2, geo: {type: "Point", coordinates: [3, 3]}}
                ]
            }
        },
        spec: {key: {"a.b.geo": "2dsphere"}, name: "2dsphereIndex"},
        expectedIndexKeys: [{"a.b.geo": "0f20000000000000"}, {"a.b.geo": "0f20002002202203"}]
    },
    {
        doc: {
            a: {
                b: [
                    {nongeo: 1, geo: {type: "Point", coordinates: [0, 0]}},
                    {nongeo: 2, geo: {type: "Point", coordinates: [3, 3]}}
                ]
            }
        },
        spec: {key: {"a.b.none": "2dsphere"}, name: "2dsphereIndex"},
        expectedIndexKeys: [{"a.b.none": null}]
    },
    {
        doc: {
            a: {
                b: [
                    {nongeo: 1, geo: {type: "Point", coordinates: [0, 0]}},
                    {nongeo: 2, geo: {type: "Point", coordinates: [3, 3]}}
                ]
            }
        },
        spec: {key: {"a.b.nongeo": 1, "a.b.geo": "2dsphere"}, name: "2dsphereIndex"},
        expectedIndexKeys: [
            {"a.b.nongeo": 1, "a.b.geo": "0f20000000000000"},
            {"a.b.nongeo": 1, "a.b.geo": "0f20002002202203"},
            {"a.b.nongeo": 2, "a.b.geo": "0f20000000000000"},
            {"a.b.nongeo": 2, "a.b.geo": "0f20002002202203"}
        ]
    },
    {
        doc: {
            a: {
                b: [
                    {
                        nongeo1: 1,
                        nongeo2: 3,
                        nongeo3: [5, 6],
                        geo: {type: "Point", coordinates: [0, 0]}
                    },
                    {nongeo1: 2, nongeo2: 4, nongeo4: 7, geo: {type: "Point", coordinates: [3, 3]}}
                ]
            }
        },
        spec: {
            key: {"a.b.geo": "2dsphere", "a.b.nongeo1": 1, "a.b.nongeo3": 1},
            name: "2dsphereIndex"
        },
        expectedIndexKeys: [
            {"a.b.geo": "0f20000000000000", "a.b.nongeo1": 1, "a.b.nongeo3": 5},
            {"a.b.geo": "0f20000000000000", "a.b.nongeo1": 1, "a.b.nongeo3": 6},
            {"a.b.geo": "0f20000000000000", "a.b.nongeo1": 2, "a.b.nongeo3": 5},
            {"a.b.geo": "0f20000000000000", "a.b.nongeo1": 2, "a.b.nongeo3": 6},
            {"a.b.geo": "0f20002002202203", "a.b.nongeo1": 1, "a.b.nongeo3": 5},
            {"a.b.geo": "0f20002002202203", "a.b.nongeo1": 1, "a.b.nongeo3": 6},
            {"a.b.geo": "0f20002002202203", "a.b.nongeo1": 2, "a.b.nongeo3": 5},
            {"a.b.geo": "0f20002002202203", "a.b.nongeo1": 2, "a.b.nongeo3": 6},
        ]
    },
    {
        doc: {
            a: {
                b: [
                    {nongeo: 1, geo: {type: "Point", coordinates: [0, 1]}},
                    {nongeo: 2, geo: {type: "Point", coordinates: [2, 3]}}
                ],
                c: [
                    {nongeo: 1, geo: {type: "Point", coordinates: [4, 5]}},
                    {nongeo: 2, geo: {type: "Point", coordinates: [6, 7]}}
                ]
            }
        },
        spec: {key: {"a.b.geo": "2dsphere", "a.c.geo": "2dsphere"}, name: "2dsphereIndex"},
        expectedIndexKeys: [
            {"a.b.geo": "0f20000033010011", "a.c.geo": "0f20002233220120"},
            {"a.b.geo": "0f20000033010011", "a.c.geo": "0f20020313220130"},
            {"a.b.geo": "0f20003113201132", "a.c.geo": "0f20002233220120"},
            {"a.b.geo": "0f20003113201132", "a.c.geo": "0f20020313220130"}
        ]
    },
    {
        doc: {
            a: {
                b: [
                    {nongeo: 1, geo: {coordinates: [0, 0]}},
                    {nongeo: 2, geo: {type: "Point", coordinates: [3, 3]}}
                ]
            }
        },
        spec: {key: {"a.b.geo": "2dsphere"}, name: "2dsphereIndex"},
        expectedErrorCode: 16755  // unknown GeoJSON type
    },

    //
    // ++++ 2dsphere_bucket index tests ++++
    // Test '$_internalIndexKey' against various shapes of 'doc' and 'spec' with 2dsphere_bucket
    // index.
    //
    {
        doc: {
            control: {version: 1},
            data: {
                geo: {
                    0: {type: "Point", coordinates: [0, 0]},
                    1: {type: "Point", coordinates: [3, 3]}
                }
            }
        },
        spec: {
            key: {"data.geo": "2dsphere_bucket"},
            "2dsphereIndexVersion": 3,
            name: "2dsphereBucketIndex"
        },
        expectedIndexKeys: [
            {"data.geo": NumberLong("1152921504875282432")},
            {"data.geo": NumberLong("1157514469887180800")}
        ]
    },
    {
        doc: {
            control: {version: 1},
            data: {
                geo1: {
                    0: {type: "Point", coordinates: [0, 0]},
                    1: {type: "Point", coordinates: [3, 3]}
                },
                geo2: {
                    0: {type: "Point", coordinates: [1, 1]},
                    1: {type: "Point", coordinates: [3, 3]}
                }
            }
        },
        spec: {
            key: {"data.geo1": "2dsphere_bucket", "data.geo2": "2dsphere_bucket"},
            "2dsphereIndexVersion": 3,
            name: "2dsphereBucketIndex"
        },
        expectedIndexKeys: [
            {
                "data.geo1": NumberLong("1152921504875282432"),
                "data.geo2": NumberLong("1153277837910736896")
            },
            {
                "data.geo1": NumberLong("1152921504875282432"),
                "data.geo2": NumberLong("1157514469887180800")
            },
            {
                "data.geo1": NumberLong("1157514469887180800"),
                "data.geo2": NumberLong("1153277837910736896")
            },
            {
                "data.geo1": NumberLong("1157514469887180800"),
                "data.geo2": NumberLong("1157514469887180800")
            }
        ]
    },
    {
        doc: {
            control: {version: 1},
            data: {
                geo1: {
                    0: {type: "Point", coordinates: [0, 0]},
                    1: {type: "Point", coordinates: [3, 3]}
                },
                geo2: {
                    0: {type: "Point", coordinates: [1, 1]},
                    1: {type: "Point", coordinates: [3, 3]}
                }
            }
        },
        spec: {
            key: {"data.geo2": "2dsphere_bucket", "data.geo1": "2dsphere_bucket"},
            "2dsphereIndexVersion": 3,
            name: "2dsphereBucketIndex"
        },
        expectedIndexKeys: [
            {
                "data.geo2": NumberLong("1153277837910736896"),
                "data.geo1": NumberLong("1152921504875282432")
            },
            {
                "data.geo2": NumberLong("1153277837910736896"),
                "data.geo1": NumberLong("1157514469887180800")
            },
            {
                "data.geo2": NumberLong("1157514469887180800"),
                "data.geo1": NumberLong("1152921504875282432")
            },
            {
                "data.geo2": NumberLong("1157514469887180800"),
                "data.geo1": NumberLong("1157514469887180800")
            }
        ]
    },
    {
        doc: {
            control: {version: 1},
            data: {
                geo1: {
                    0: {type: "Point", coordinates: [0, 0]},
                    1: {type: "Point", coordinates: [3, 3]}
                },
                geo2: {
                    0: {type: "Point", coordinates: [1, 1]},
                    1: {type: "Point", coordinates: [3, 3]}
                }
            }
        },
        spec: {
            key: {"data.geo1": "2dsphere_bucket", "data.geo2": 1},
            "2dsphereIndexVersion": 3,
            name: "2dsphereBucketIndex"
        },
        expectedIndexKeys: [
            {
                "data.geo1": NumberLong("1152921504875282432"),
                "data.geo2": {
                    "0": {"type": "Point", "coordinates": [1, 1]},
                    "1": {"type": "Point", "coordinates": [3, 3]}
                }
            },
            {
                "data.geo1": NumberLong("1157514469887180800"),
                "data.geo2": {
                    "0": {"type": "Point", "coordinates": [1, 1]},
                    "1": {"type": "Point", "coordinates": [3, 3]}
                }
            }
        ]
    },
    {
        doc: {
            control: {version: 1},
            data: {
                geo: {
                    0: {type: "Point", coordinates: [0, 0]},
                    1: {type: "Point", coordinates: [3, 3]}
                }
            }
        },
        spec: {
            key: {"data.none1": "2dsphere_bucket"},
            "2dsphereIndexVersion": 3,
            name: "2dsphereBucketIndex"
        },
        expectedIndexKeys: []
    },
    {
        doc: {
            control: {version: 1},
            data: {geo: {0: {coordinates: [0, 0]}, 1: {type: "Point", coordinates: [3, 3]}}}
        },
        spec: {
            key: {"data.geo": "2dsphere_bucket"},
            "2dsphereIndexVersion": 3,
            name: "2dsphereBucketIndex"
        },
        expectedErrorCode: 183934  // Can't extract geo keys: unknown GeoJSON type.
    },
    {
        doc: {
            data: {
                geo: {
                    0: {type: "Point", coordinates: [0, 0]},
                    1: {type: "Point", coordinates: [3, 3]}
                }
            }
        },
        spec: {
            key: {"data.geo": "2dsphere_bucket"},
            "2dsphereIndexVersion": 3,
            name: "2dsphereBucketIndex"
        },
        expectedErrorCode:
            6540600  // Time-series bucket documents must have 'control' object present.
    },

    //
    // ++++ text index tests ++++
    // Test '$_internalIndexKey' against various shapes of 'doc' and 'spec' with text index.
    //
    {
        doc: {places: "Dublin London", food: "Salad, Soups"},
        spec: {
            key: {places: "text", food: "text"},
            name: "textIndex",
            weights: {places: 2, food: 1},
            "default_language": "english",
            textIndexVersion: 3
        },
        expectedIndexKeys: [
            {"places": "Dublin London", "food": "Salad, Soups", "term": "dublin", "weight": 1.5},
            {"places": "Dublin London", "food": "Salad, Soups", "term": "london", "weight": 1.5},
            {"places": "Dublin London", "food": "Salad, Soups", "term": "salad", "weight": 0.75},
            {"places": "Dublin London", "food": "Salad, Soups", "term": "soup", "weight": 0.75}
        ]
    },
    {
        doc: {places: "Dublin New-York Toronto", food: "Salad, Soups, Cakes"},
        spec: {
            key: {food: "text", places: "text"},
            name: "textIndex",
            weights: {places: 2, food: 1},
            "default_language": "english",
            textIndexVersion: 3
        },
        expectedIndexKeys: [
            {
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto",
                "term": "cake",
                "weight": 0.6666666666666666
            },
            {
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto",
                "term": "dublin",
                "weight": 1.25
            },
            {
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto",
                "term": "new",
                "weight": 1.25
            },
            {
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto",
                "term": "salad",
                "weight": 0.6666666666666666
            },
            {
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto",
                "term": "soup",
                "weight": 0.6666666666666666
            },
            {
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto",
                "term": "toronto",
                "weight": 1.25
            },
            {
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto",
                "term": "york",
                "weight": 1.25
            }

        ]
    },
    {
        doc: {places: "Dublin New-York Toronto", food: "Salad, Soups, Cakes"},
        spec: {
            key: {_fts: "text", food: "text", places: "text"},
            name: "textIndex",
            weights: {places: 2, food: 1},
            "default_language": "english",
            textIndexVersion: 3
        },
        expectedIndexKeys: [
            {
                "term": "cake",
                "weight": 0.6666666666666666,
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto"
            },
            {
                "term": "dublin",
                "weight": 1.25,
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto"
            },
            {
                "term": "new",
                "weight": 1.25,
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto"
            },
            {
                "term": "salad",
                "weight": 0.6666666666666666,
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto"
            },
            {
                "term": "soup",
                "weight": 0.6666666666666666,
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto"
            },
            {
                "term": "toronto",
                "weight": 1.25,
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto"
            },
            {
                "term": "york",
                "weight": 1.25,
                "food": "Salad, Soups, Cakes",
                "places": "Dublin New-York Toronto"
            }

        ]
    },
    {
        doc: {
            places: "Dublin New-York Toronto",
            food: "Salad, Soups, Cakes",
            description: "This is a test with text index",
            sports: "Cricket Football Tennis Baseball"
        },
        spec: {
            key: {
                food: "text",
                sports: "text",
                _ftsx: 1,
                places: "text",
                _fts: "text",
                description: "text"
            },
            name: "textIndex",
            weights: {places: 2, description: 1, food: 1, sports: 1},
            "default_language": "english",
            textIndexVersion: 3
        },
        expectedIndexKeys: [
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "basebal",
                "weight": 0.625,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "cake",
                "weight": 0.6666666666666666,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "cricket",
                "weight": 0.625,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "dublin",
                "weight": 1.25,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "footbal",
                "weight": 0.625,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "index",
                "weight": 0.6666666666666666,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "new",
                "weight": 1.25,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "salad",
                "weight": 0.6666666666666666,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "soup",
                "weight": 0.6666666666666666,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "tenni",
                "weight": 0.625,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "test",
                "weight": 0.6666666666666666,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "text",
                "weight": 0.6666666666666666,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "toronto",
                "weight": 1.25,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            },
            {
                "food": "Salad, Soups, Cakes",
                "sports": "Cricket Football Tennis Baseball",
                "term": "york",
                "weight": 1.25,
                "places": "Dublin New-York Toronto",
                "description": "This is a test with text index"
            }
        ]
    },

    //
    // ++++ wildcard index tests ++++
    // Test '$_internalIndexKey' against various shapes of 'doc' and 'spec' with wildcard index.
    //
    {
        doc: {a: {b: "one", c: 2}},
        spec: {key: {"$**": 1}, name: "wildcardIndex"},
        expectedIndexKeys: [{"a.b": "one"}, {"a.c": 2}]
    },
    {
        doc: {a: {b: "one", c: 2}},
        spec: {key: {"d.$**": 1}, name: "wildcardIndex"},
        expectedIndexKeys: []
    },
    {
        doc: {
            product_name: "Spy Coat",
            product_attributes:
                {material: ["Tweed", "Wool", "Leather"], size: {length: 72, units: "inches"}}
        },
        spec: {key: {"product_attributes.$**": 1}, name: "wildcardIndex"},
        expectedIndexKeys: [
            {"product_attributes.material": "Leather"},
            {"product_attributes.material": "Tweed"},
            {"product_attributes.material": "Wool"},
            {"product_attributes.size.length": 72},
            {"product_attributes.size.units": "inches"}
        ]
    },
    {
        // TODO SERVER-68303: Update this test case when the feature flag is removed.
        doc: {a: {b: "one", c: 2}},
        spec: {key: {"$**": 1, "a.b": 1}, name: "wildcardIndex"},
        expectedErrorCode:
            ErrorCodes.CannotCreateIndex,  // wildcard indexes do not allow compounding.
        skipCWI: true
    },
];

// Run each test scenario and verify that the '$_internalIndexKey' returns the
// expected response.
testScenarios.forEach(testScenario => {
    jsTestLog("Testing scenario: " + tojson(testScenario));

    // TODO SERVER-68303: Remove this.
    if (testScenario.hasOwnProperty("skipCWI") && testScenario["skipCWI"] === true &&
        FeatureFlagUtil.isEnabled(db, "CompoundWildcardIndexes")) {
        jsTestLog("Skipping because compound wildcard indexes are enabled");
        return;
    }

    // Drop the collection so the '$$ROOT' does not pick documents from the last test
    // scenario.
    collection.drop();

    // Insert the document to the collection if the field 'doc' exists in the
    // test-scenario dictionary.
    if (testScenario.doc) {
        assert.commandWorked(collection.insert(testScenario.doc));
    }

    // Prepare the pipeline that consists of the '$_internalIndexKey' expression. The
    // 'doc' field corresponds to the '$$ROOT', as such there must be only document in
    // the collection, so that the aggregate command returns only one key document for
    // each test scenario.
    let internalIndexKeySpec = {};
    if (testScenario.doc) {
        internalIndexKeySpec["doc"] = "$$ROOT";
    }
    if (testScenario.spec) {
        internalIndexKeySpec["spec"] = testScenario.spec;
    }
    const pipeline = [{$replaceRoot: {newRoot: {_id: {$_internalIndexKey: internalIndexKeySpec}}}}];

    // If the 'expectedIndexKeys' field is present, then the aggregate command must
    // succeed and the
    // '$_internalIndexKey' must return expected keys document. Otherwise the
    // 'expectedErrorCode' must be present and the aggregate command in such case must
    // throw an exception.
    if (testScenario.expectedIndexKeys) {
        assert.eq(collection.aggregate(pipeline).toArray()[0],
                  {_id: testScenario.expectedIndexKeys},
                  testScenario);
    } else {
        assert.throwsWithCode(() => collection.aggregate(pipeline).toArray(),
                              testScenario.expectedErrorCode);
    }
});
})();
