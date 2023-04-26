/**
 * Tests that the validate checkBSONConformance option works with various BSON types.
 */

(function() {
const coll = db.validate_bson_types;

assert.commandWorked(coll.insert({a: 1.0}));        // double
assert.commandWorked(coll.insert({b: "abc"}));      // string
assert.commandWorked(coll.insert({c: {x: 1}}));     // object
assert.commandWorked(coll.insert({d: [1, 2, 3]}));  // array
assert.commandWorked(coll.insert({
    e: BinData(2, "KwAAAFRoZSBxdWljayBicm93biBmb3gganVtcHMgb3ZlciB0aGUgbGF6eSBkb2c=")
}));                                                                           // binData
assert.commandWorked(coll.insert({f: undefined}));                             // undefined
assert.commandWorked(coll.insert({g: ObjectId("dbdbdbdbdbdbdbdbdbdbdbdb")}));  // objectId
assert.commandWorked(coll.insert({h: true}));                                  // boolean
assert.commandWorked(coll.insert({i: ISODate("2013-12-11T19:38:24.055Z")}));   // UTC
assert.commandWorked(coll.insert({j: null}));                                  // null
assert.commandWorked(coll.insert({k: RegExp("a")}));                           // regex
assert.commandWorked(
    coll.insert({l: DBPointer("foo", ObjectId("bbbbbbbbbbbbbbbbbbbbbbbb"))}));  // DBPointer
assert.commandWorked(coll.insert({m: Code("noScope")}));                        // code
assert.commandWorked(coll.insert({n: Code('function(){return 1;}', {})}));      // code w/ scope
assert.commandWorked(coll.insert({o: 3}));                                      // int
assert.commandWorked(coll.insert({p: Timestamp(1, 2)}));                        // timestamp
assert.commandWorked(coll.insert({q: NumberLong(6)}));                          // 64-bit int
assert.commandWorked(coll.insert({r: NumberDecimal("2.0")}));                   // decimal 128
assert.commandWorked(coll.insert({s: MinKey()}));                               // MinKey
assert.commandWorked(coll.insert({t: MaxKey()}));                               // MaxKey

assert.commandWorked(coll.validate({checkBSONConformance: true}));
})();
