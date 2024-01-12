// Tests the behavior of $_internalKeyStringValue when used in agg expressions.

const coll = db[jsTestName()];

// Testing behavior for basic data types.
coll.drop();
let docs = [
    {_id: 0, a: NumberInt(1)},
    {_id: 1, a: NumberLong(2)},
    {_id: 2, a: 3.0},
    {_id: 3, a: NumberDecimal("4.000000")},
    {_id: 4, a: "abc"},
    {_id: 5, a: ISODate("2024-01-01")},
    {_id: 6, a: [1, 2, 3]},
    {_id: 7, a: {b: 1, c: 2, d: 3}},
];
assert.commandWorked(coll.insert(docs));

let results =
    coll.aggregate(
            [{$sort: {_id: 1}}, {$addFields: {b: {$_internalKeyStringValue: {input: "$a"}}}}])
        .toArray();
assert.eq(results,
          [
              {_id: 0, a: NumberInt(1), b: BinData(0, "KwIE")},
              {_id: 1, a: NumberLong(2), b: BinData(0, "KwQE")},
              {_id: 2, a: 3.0, b: BinData(0, "KwYE")},
              {_id: 3, a: NumberDecimal("4.000000"), b: BinData(0, "KwgE")},
              {_id: 4, a: "abc", b: BinData(0, "PGFiYwAE")},
              {_id: 5, a: ISODate("2024-01-01"), b: BinData(0, "eIAAAYzCUfQABA==")},
              {_id: 6, a: [1, 2, 3], b: BinData(0, "UCsCKwQrBgAE")},
              {_id: 7, a: {b: 1, c: 2, d: 3}, b: BinData(0, "Rh5iACsCHmMAKwQeZAArBgAE")},
          ],
          results);

// Testing behavior for same numeric values of different types.
assert(coll.drop());
docs = [
    {_id: 0, a: NumberInt(1)},
    {_id: 1, a: NumberLong(1)},
    {_id: 2, a: 1.0},
    {_id: 3, a: NumberDecimal("1.000")},
    {_id: 4, a: NumberDecimal("1.000000")},
];
assert.commandWorked(coll.insert(docs));

results = coll.aggregate(
                  [{$sort: {_id: 1}}, {$addFields: {b: {$_internalKeyStringValue: {input: "$a"}}}}])
              .toArray();
assert.eq(docs.length, results.length, results);
assert(results.every(result => bsonWoCompare(result.b, results[0].b) === 0), results);

// Testing behavior for same numeric values of different types inside object.
assert(coll.drop());
docs = [
    {_id: 0, a: {b: NumberInt(1)}},
    {_id: 1, a: {b: 1.0}},
    {_id: 2, a: {b: NumberDecimal("1.000")}},
    {_id: 3, a: {b: NumberDecimal("1.000000")}},
];
assert.commandWorked(coll.insert(docs));

results = coll.aggregate(
                  [{$sort: {_id: 1}}, {$addFields: {c: {$_internalKeyStringValue: {input: "$a"}}}}])
              .toArray();
assert.eq(docs.length, results.length, results);
assert(results.every(result => bsonWoCompare(result.c, results[0].c) === 0), results);

// Testing behavior for close numeric values.
assert(coll.drop());
docs = [
    {_id: 0, a: 1},
    {_id: 1, a: 1.00001},
];
assert.commandWorked(coll.insert(docs));

results = coll.aggregate(
                  [{$sort: {_id: 1}}, {$addFields: {b: {$_internalKeyStringValue: {input: "$a"}}}}])
              .toArray();
assert.eq(2, results.length, results);
assert(bsonWoCompare(results[0].b, results[1].b) !== 0, results);

// Testing behavior for large numeric values.
assert(coll.drop());
docs = [
    {_id: 0, a: 1e20},
    {_id: 1, a: NumberDecimal("1e21")},
];
assert.commandWorked(coll.insert(docs));

results =
    coll.aggregate([
            {$sort: {_id: 1}},
            {
                $addFields:
                    {b: {$_internalKeyStringValue: {input: "$a"}}, c: {$toHashedIndexKey: "$a"}}
            }
        ])
        .toArray();
assert.eq(2, results.length, results);
assert(bsonWoCompare(results[0].b, results[1].b) !== 0, results);
// $toHashedIndexKey hashes large numbers greater than 2^63 to the same result.
assert(bsonWoCompare(results[0].c, results[1].c) === 0, results);

// Testing behavior for strings under case-sensitive collation that doesn't match.
assert(coll.drop());
docs = [
    {_id: 0, a: "aAa"},
    {_id: 1, a: "AaA"},
];
assert.commandWorked(coll.insert(docs));

results = coll.aggregate([
                  {$sort: {_id: 1}},
                  {
                      $addFields: {
                          b: {
                              $_internalKeyStringValue:
                                  {input: "$a", collation: {locale: "en", strength: 3}}
                          }
                      }
                  }
              ])
              .toArray();
assert.eq(2, results.length, results);
assert(bsonWoCompare(results[0].b, results[1].b) !== 0, results);

// Testing behavior for strings under case-sensitive collation that matches.
assert(coll.drop());
docs = [
    {_id: 0, a: "aAa"},
    {_id: 1, a: "aAa"},
];
assert.commandWorked(coll.insert(docs));

results = coll.aggregate([
                  {$sort: {_id: 1}},
                  {
                      $addFields: {
                          b: {
                              $_internalKeyStringValue:
                                  {input: "$a", collation: {locale: "en", strength: 3}}
                          }
                      }
                  }
              ])
              .toArray();
assert.eq(docs.length, results.length, results);
assert(results.every(result => bsonWoCompare(result.c, results[0].c) === 0), results);

// Testing behavior for strings under case-insensitive collation that doesn't match.
assert(coll.drop());
docs = [
    {_id: 0, a: "aAa"},
    {_id: 1, a: "aBa"},
];
assert.commandWorked(coll.insert(docs));

results = coll.aggregate([
                  {$sort: {_id: 1}},
                  {
                      $addFields: {
                          b: {
                              $_internalKeyStringValue:
                                  {input: "$a", collation: {locale: "en", strength: 1}}
                          }
                      }
                  }
              ])
              .toArray();
assert.eq(2, results.length, results);
assert(bsonWoCompare(results[0].b, results[1].b) !== 0, results);

// Testing behavior for strings under case-insensitive collation that matches.
assert(coll.drop());
docs = [
    {_id: 0, a: "aAa"},
    {_id: 1, a: "AaA"},
];
assert.commandWorked(coll.insert(docs));

results = coll.aggregate([
                  {$sort: {_id: 1}},
                  {
                      $addFields: {
                          b: {
                              $_internalKeyStringValue:
                                  {input: "$a", collation: {locale: "en", strength: 1}}
                          }
                      }
                  }
              ])
              .toArray();
assert.eq(docs.length, results.length, results);
assert(results.every(result => bsonWoCompare(result.b, results[0].b) === 0), results);
