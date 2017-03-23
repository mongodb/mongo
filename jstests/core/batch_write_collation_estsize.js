// Tests that the update and delete batch write operations account for the size of the collation
// specification in the write operation document.

(function() {
    "use strict";

    // Setup the test collection.
    db.batch_write_collation_estsize.drop();
    assert.writeOK(db.batch_write_collation_estsize.insert({str: "FOO"}));

    if (db.getMongo().writeMode() !== "commands") {
        // Cannot use the bulk API to set a collation when using legacy write ops.
        let bulk;

        // Test updateOne unordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeUnorderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).updateOne({
                str: "BAR"
            });
        });

        // Test update unordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeUnorderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).update({str: "BAR"});
        });

        // Test replaceOne unordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeUnorderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).replaceOne({
                str: "BAR"
            });
        });

        // Test removeOne unordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeUnorderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).removeOne();
        });

        // Test remove unordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeUnorderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).remove();
        });

        // Test updateOne ordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeOrderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).updateOne({
                str: "BAR"
            });
        });

        // Test update ordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeOrderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).update({str: "BAR"});
        });

        // Test replaceOne ordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeOrderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).replaceOne({
                str: "BAR"
            });
        });

        // Test removeOne ordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeOrderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).removeOne();
        });

        // Test remove ordered bulk write operation with collation specification.
        bulk = db.batch_write_collation_estsize.initializeOrderedBulkOp();
        assert.throws(() => {
            bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).remove();
        });
    } else {
        // Setup the bulk write response variable.
        let res;

        // Test updateOne bulk write operation with collation specification.
        res = db.batch_write_collation_estsize.bulkWrite([{
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
                    backwards: false
                }
            }
        }]);
        assert.eq(1, res.matchedCount);

        // Test updateMany bulk write operation with collation specification.
        res = db.batch_write_collation_estsize.bulkWrite([{
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
                    backwards: false
                }
            }
        }]);
        assert.eq(1, res.matchedCount);

        // Test replaceOne bulk write operation with collation specification.
        res = db.batch_write_collation_estsize.bulkWrite([{
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
                    backwards: false
                }
            }
        }]);
        assert.eq(1, res.matchedCount);

        // Test deleteMany bulk write operation with collation specification.
        res = db.batch_write_collation_estsize.bulkWrite([{
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
                    backwards: false
                }
            }
        }]);
        assert.eq(1, res.deletedCount);

        // Reinsert a document to test deleteMany bulk write operation.
        assert.writeOK(db.batch_write_collation_estsize.insert({str: "FOO"}));

        // Test deleteMany bulk write operation with collation specification.
        res = db.batch_write_collation_estsize.bulkWrite([{
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
                    backwards: false
                }
            }
        }]);
        assert.eq(1, res.deletedCount);
    }
})();
