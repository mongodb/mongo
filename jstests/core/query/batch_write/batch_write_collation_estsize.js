// @tags: [requires_multi_updates, requires_non_retryable_writes]

// Tests that the update and delete batch write operations account for the size of the collation
// specification in the write operation document.

// Setup the test collection.
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({str: "FOO"}));

// Test updateOne bulk write operation with collation specification.
let res = coll.bulkWrite([
    {
        updateOne: {
            filter: {str: "FOO"},
            update: {$set: {str: "BAR"}},
            collation: {
                locale: "en_US",
                caseLevel: false,
                caseFirst: "off",
                strength: 3,
                numericOrdering: false,
                alternate: "non-ignorable",
                maxVariable: "punct",
                normalization: false,
                backwards: false,
            },
        },
    },
]);
assert.eq(1, res.matchedCount);

// Test updateMany bulk write operation with collation specification.
res = coll.bulkWrite([
    {
        updateMany: {
            filter: {str: "BAR"},
            update: {$set: {str: "FOO"}},
            collation: {
                locale: "en_US",
                caseLevel: false,
                caseFirst: "off",
                strength: 3,
                numericOrdering: false,
                alternate: "non-ignorable",
                maxVariable: "punct",
                normalization: false,
                backwards: false,
            },
        },
    },
]);
assert.eq(1, res.matchedCount);

// Test replaceOne bulk write operation with collation specification.
res = coll.bulkWrite([
    {
        replaceOne: {
            filter: {str: "FOO"},
            replacement: {str: "BAR"},
            collation: {
                locale: "en_US",
                caseLevel: false,
                caseFirst: "off",
                strength: 3,
                numericOrdering: false,
                alternate: "non-ignorable",
                maxVariable: "punct",
                normalization: false,
                backwards: false,
            },
        },
    },
]);
assert.eq(1, res.matchedCount);

// Test deleteMany bulk write operation with collation specification.
res = coll.bulkWrite([
    {
        deleteOne: {
            filter: {str: "BAR"},
            collation: {
                locale: "en_US",
                caseLevel: false,
                caseFirst: "off",
                strength: 3,
                numericOrdering: false,
                alternate: "non-ignorable",
                maxVariable: "punct",
                normalization: false,
                backwards: false,
            },
        },
    },
]);
assert.eq(1, res.deletedCount);

// Reinsert a document to test deleteMany bulk write operation.
assert.commandWorked(coll.insert({str: "FOO"}));

// Test deleteMany bulk write operation with collation specification.
res = coll.bulkWrite([
    {
        deleteMany: {
            filter: {str: "FOO"},
            collation: {
                locale: "en_US",
                caseLevel: false,
                caseFirst: "off",
                strength: 3,
                numericOrdering: false,
                alternate: "non-ignorable",
                maxVariable: "punct",
                normalization: false,
                backwards: false,
            },
        },
    },
]);
assert.eq(1, res.deletedCount);
