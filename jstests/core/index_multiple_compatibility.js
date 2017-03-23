// Test that multiple indexes behave correctly together.
(function() {
    'use strict';
    var coll = db.index_multiple_compatibility;
    coll.drop();

    const enUSStrength1 = {locale: "en_US", strength: 1};
    const enUSStrength2 = {locale: "en_US", strength: 2};
    const enUSStrength3 = {locale: "en_US", strength: 3};

    /**
     * testIndexCompat runs a series of operations on two indexes to ensure that the two behave
     * properly in combination.
     *
     * 'index1' and 'index2' take a document in the following format:
     *
     * {
     *     index: {key: Document, name: String, collation: Document, options...}
     *     doc: Document
     * }
     *
     * The 'index' key indicates the index to create, and 'doc' (optional) indicates a document to
     * insert in the collection, and look for in *only* this index.  The 'index' key will be passed
     * directly to the createIndexes command.
     *
     * 'both' optionally provides a document to insert into the collection, and expect in both
     * indexes.
     *
     * - Create both indexes.
     * - Insert document in index1.
     * - Check that it is present in index1, and absent in index2, using find and a hint.
     * - Insert document in index2.
     * - Check that it is present in index2, and absent in index1, using find and a hint.
     * - Insert the document 'both', if it is provided.  Check that it is inserted in both indexes.
     * - Delete documents ensuring they are removed from the appropriate indexes.
     */
    function testIndexCompat(coll, index1, index2, both) {
        coll.drop();

        assert(index1.hasOwnProperty('index'));
        assert(index2.hasOwnProperty('index'));

        assert.commandWorked(
            db.runCommand({createIndexes: coll.getName(), indexes: [index1.index, index2.index]}));

        // Check index 1 document.
        if (index1.hasOwnProperty('doc')) {
            assert.writeOK(coll.insert(index1.doc));
            assert.eq(coll.find(index1.doc).hint(index1.index.name).itcount(), 1);
            assert.eq(coll.find(index1.doc).hint(index2.index.name).itcount(), 0);
        }

        // Check index 2 document.
        if (index2.hasOwnProperty('doc')) {
            assert.writeOK(coll.insert(index2.doc));
            assert.eq(coll.find(index2.doc).hint(index2.index.name).itcount(), 1);
            assert.eq(coll.find(index2.doc).hint(index1.index.name).itcount(), 0);
        }

        // Check for present of both in both index1 and index2.
        if (typeof both !== "undefined") {
            assert.writeOK(coll.insert(both));
            assert.eq(coll.find(both).hint(index1.index.name).itcount(), 1);
            assert.eq(coll.find(both).hint(index2.index.name).itcount(), 1);
        }

        // Remove index 1 document.
        if (index1.hasOwnProperty('doc')) {
            assert.writeOK(coll.remove(index1.doc));
            assert.eq(coll.find(index1.doc).hint(index1.index.name).itcount(), 0);
        }

        // Remove index 2 document.
        if (index2.hasOwnProperty('doc')) {
            assert.writeOK(coll.remove(index2.doc));
            assert.eq(coll.find(index2.doc).hint(index2.index.name).itcount(), 0);
        }

        // Remove both.
        if (typeof both !== "undefined") {
            assert.writeOK(coll.remove(both));
            assert.eq(coll.find(both).hint(index1.index.name).itcount(), 0);
            assert.eq(coll.find(both).hint(index2.index.name).itcount(), 0);
        }
    }

    // Two identical partial indexes.
    testIndexCompat(coll,
                    {
                      index: {
                          key: {a: 1},
                          name: "a1",
                          collation: enUSStrength1,
                          partialFilterExpression: {a: {$type: 'string'}}
                      }
                    },
                    {
                      index: {
                          key: {a: 1},
                          name: "a2",
                          collation: enUSStrength2,
                          partialFilterExpression: {a: {$type: 'string'}}
                      }
                    },
                    {a: "A"});

    // Two non-overlapping partial indexes.
    testIndexCompat(coll,
                    {
                      index: {
                          key: {a: 1},
                          name: "a1",
                          collation: enUSStrength1,
                          partialFilterExpression: {a: {$lt: 10}}
                      },
                      doc: {a: 5}
                    },
                    {
                      index: {
                          key: {a: 1},
                          name: "a2",
                          collation: enUSStrength2,
                          partialFilterExpression: {a: {$gt: 20}}
                      },
                      doc: {a: 25}
                    });

    // Two partially overlapping partial indexes.
    testIndexCompat(coll,
                    {
                      index: {
                          key: {a: 1},
                          name: "a1",
                          collation: enUSStrength1,
                          partialFilterExpression: {a: {$lt: 10}},
                      },
                      doc: {a: -5}
                    },
                    {
                      index: {
                          key: {a: 1},
                          name: "a2",
                          collation: enUSStrength2,
                          partialFilterExpression: {a: {$gte: 0}}
                      },
                      doc: {a: 15}
                    },
                    {a: 5});

    // A partial and sparse index.
    testIndexCompat(
        coll,
        {
          index:
              {key: {a: 1}, name: "a1", collation: enUSStrength1, partialFilterExpression: {b: 0}},
          doc: {b: 0}
        },
        {
          index: {key: {a: 1}, name: "a2", collation: enUSStrength2, sparse: true},
          doc: {a: 5, b: 1}
        },
        {a: -1, b: 0});

    // A sparse and non-sparse index.
    testIndexCompat(
        coll,
        {
          index: {key: {a: 1}, name: "a1", collation: enUSStrength1, sparse: true},
        },
        {index: {key: {a: 1}, name: "a2", collation: enUSStrength2, sparse: false}, doc: {b: 0}},
        {a: 1});

    // A unique index and non-unique index.
    testIndexCompat(coll,
                    {
                      index: {key: {a: 1}, name: "unique", collation: enUSStrength1, unique: true},
                    },
                    {index: {key: {a: 1}, name: "reg", collation: enUSStrength2, unique: false}},
                    {a: "foo"});

    // Test that unique constraints are still enforced.
    assert.writeOK(coll.insert({a: "f"}));
    assert.writeError(coll.insert({a: "F"}));

    // A unique partial index and non-unique index.
    testIndexCompat(
        coll,
        {
          index: {
              key: {a: 1},
              name: "unique",
              collation: enUSStrength1,
              unique: true,
              partialFilterExpression: {a: {$type: 'number'}}
          }
        },
        {index: {key: {a: 1}, name: "a", collation: enUSStrength2, unique: false}, doc: {a: "foo"}},
        {a: 5});

    assert.writeOK(coll.insert({a: 5}));
    // Test that uniqueness is only enforced by the partial index.
    assert.writeOK(coll.insert({a: "foo"}));
    assert.writeOK(coll.insert({a: "foo"}));
    assert.writeError(coll.insert({a: 5}));

    // Two unique indexes with different collations.
    testIndexCompat(coll,
                    {index: {key: {a: 1}, name: "a1", collation: enUSStrength1, unique: true}},
                    {index: {key: {a: 1}, name: "a2", collation: enUSStrength3, unique: true}},
                    {a: "a"});

    // Unique enforced on both indexes.
    assert.writeOK(coll.insert({a: "a"}));
    assert.writeError(coll.insert({a: "a"}));
    assert.writeError(coll.insert({a: "A"}));

    // A unique and sparse index.
    testIndexCompat(
        coll,
        {
          index: {key: {a: 1}, name: "a1", collation: enUSStrength1, unique: true, sparse: true},
        },
        {index: {key: {a: 1}, name: "a2", collation: enUSStrength2, unique: false}, doc: {b: 0}},
        {a: "a"});

    assert.writeOK(coll.insert({a: "a"}));
    assert.writeOK(coll.insert({}));
    assert.writeOK(coll.insert({}));
    assert.writeError(coll.insert({a: "a"}));
})();
