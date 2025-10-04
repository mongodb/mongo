/**
 * This test ensures that basic eq predicates work with a BinData value.
 */
const coll = db.jstests_bindata_find_eq;

function testEqOnlyBinData(subtype, blob, expectedCount) {
    assert.eq(expectedCount, coll.find({"a": {$eq: BinData(subtype, blob)}}).itcount());
}

coll.drop();

const docs = [
    {a: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
    {a: BinData(0, "AQAAAAEBAAVlbl9VSwAAAAAAAAhv")},
    {a: BinData(0, "AQAAAAEBAAVlbl9VSwAAAAAAAAhz")},
    {a: BinData(0, "////////////////////////////")},
    {a: BinData(0, "ZG9n")},
    {a: BinData(0, "JA4A8gAxqTwciCuF5GGzAA==")},
    {a: BinData(1, "JA4A8gAxqTwciCuF5GGzAA==")},
    {a: BinData(0, "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIHRoZSBsYXp5IGRvZw==")},
    {a: BinData(2, "KwAAAFRoZSBxdWljayBicm93biBmb3gganVtcHMgb3ZlciB0aGUgbGF6eSBkb2c=")},
];
assert.commandWorked(coll.insert(docs));

// Test basic queries.
testEqOnlyBinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 1);
testEqOnlyBinData(0, "AQAAAAEBAAVlbl9VSwAAAAAAAAhv", 1);
testEqOnlyBinData(0, "AQAAAAEBAAVlbl9VSwAAAAAAAAhz", 1);
testEqOnlyBinData(0, "////////////////////////////", 1);

testEqOnlyBinData(0, "ZG9n", 1);
testEqOnlyBinData(0, "JA4A8gAxqTwciCuF5GGzAA==", 1);
testEqOnlyBinData(1, "JA4A8gAxqTwciCuF5GGzAA==", 1);
testEqOnlyBinData(0, "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIHRoZSBsYXp5IGRvZw==", 1);
testEqOnlyBinData(2, "KwAAAFRoZSBxdWljayBicm93biBmb3gganVtcHMgb3ZlciB0aGUgbGF6eSBkb2c=", 1);

// Test BinData records with different subtypes.
const docs2 = [
    {a: BinData(1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
    {a: BinData(2, "KwAAAFRoZSBxdWljayBicm93biBmb3gganVtcHMgb3ZlciB0aGUgbGF6eSBkb2c=")},
    {a: BinData(3, "OEJTfmD8twzaj/LPKLIVkA==")},
    {a: BinData(4, "OEJTfmD8twzaj/LPKLIVkA==")},
    {a: BinData(5, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
    {a: BinData(6, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
];
assert.commandWorked(coll.insert(docs2));

testEqOnlyBinData(1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 1);
testEqOnlyBinData(2, "KwAAAFRoZSBxdWljayBicm93biBmb3gganVtcHMgb3ZlciB0aGUgbGF6eSBkb2c=", 2);
testEqOnlyBinData(3, "OEJTfmD8twzaj/LPKLIVkA==", 1);
testEqOnlyBinData(4, "OEJTfmD8twzaj/LPKLIVkA==", 1);
testEqOnlyBinData(5, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 1);
testEqOnlyBinData(6, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 1);

// Test queries with subtypes that yield empty results.
testEqOnlyBinData(8, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 0);
testEqOnlyBinData(9, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 0);

// Test queries that return nothing.
testEqOnlyBinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAB", 0);
testEqOnlyBinData(0, "", 0);

// Test that we can find multiple docs with the same BinData.
const duplicates = [
    {a: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
    {a: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
    {a: BinData(1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
    {a: BinData(1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
];
assert.commandWorked(coll.insert(duplicates));

testEqOnlyBinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 3);
testEqOnlyBinData(1, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA", 3);

assert(coll.drop());
