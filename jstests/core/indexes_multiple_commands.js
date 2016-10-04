// Test that commands behave correctly under the presence of multiple indexes with the same key
// pattern.
(function() {
    'use strict';

    var coll = db.indexes_multiple_commands;
    var usingWriteCommands = db.getMongo().writeMode() === "commands";

    /**
     * Assert that the result of the index creation ('cmd') indicates that 'numIndexes' were
     * created.
     *
     * If omitted, 'numIndexes' defaults to 1.
     *
     * @param cmd {Function} A function to execute that attempts to create indexes.
     * @param numIndexes {Number} The expected number of indexes that cmd creates.
     */
    function assertIndexesCreated(cmd, numIndexes) {
        var cmdResult;

        if (typeof numIndexes === "undefined") {
            numIndexes = 1;
        }

        if (usingWriteCommands) {
            cmdResult = cmd();
            assert.commandWorked(cmdResult);
            var isShardedNS = cmdResult.hasOwnProperty('raw');
            if (isShardedNS) {
                cmdResult = cmdResult['raw'][Object.getOwnPropertyNames(cmdResult['raw'])[0]];
            }
            assert.eq(cmdResult.numIndexesAfter - cmdResult.numIndexesBefore,
                      numIndexes,
                      tojson(cmdResult));
        } else {
            var nIndexesBefore = coll.getIndexes().length;
            cmdResult = cmd();
            assert.commandWorked(cmdResult);
            var nIndexesAfter = coll.getIndexes().length;
            assert.eq(nIndexesAfter - nIndexesBefore, numIndexes, tojson(coll.getIndexes()));
        }
    }

    /**
     * Assert that the result of the index create command indicates no indexes were created since
     * the indexes were the same (collation and key pattern matched).
     *
     * (Index creation succeeds if none are created, as long as no options conflict.)
     *
     * @param {Function} A function to execute that attempts to create indexes.
     */
    function assertIndexNotCreated(cmd) {
        assertIndexesCreated(cmd, 0);
    }

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName()));

    // Test that multiple indexes with the same key pattern and different collation can be created.

    assertIndexesCreated(() => coll.createIndex({a: 1}, {name: "a_1"}));
    // The requested index already exists, but with a different name, so the index is not created.
    assertIndexNotCreated(() => coll.createIndex({a: 1}, {name: "a_1:1"}));

    // Indexes with different collations and the same key pattern are allowed if the names are
    // not the same.
    assertIndexesCreated(() => coll.createIndex({a: 1}, {name: "fr", collation: {locale: "fr"}}));
    assertIndexesCreated(
        () => coll.createIndex({a: 1}, {name: "en_US", collation: {locale: "en_US"}}));

    // The requested index doesn't yet exist, but the name is used, so this command fails.
    assert.commandFailed(coll.createIndex({a: 1}, {name: "a_1", collation: {locale: "en_US"}}));

    // The requested index already exists with a different name, so the index is not created.
    assertIndexNotCreated(() => coll.createIndex({a: 1}, {name: "fr2", collation: {locale: "fr"}}));

    // Options can differ on indexes with different collations.
    assertIndexesCreated(
        () => coll.createIndex(
            {a: 1}, {name: "fr1_sparse", collation: {locale: "fr", strength: 1}, sparse: true}));

    // The requested index already exists, but with different options, so the command fails.
    assert.commandFailed(
        coll.createIndex({a: 1}, {name: "fr_sparse", collation: {locale: "fr"}, sparse: true}));

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName()));

    // Multiple non-conflicting indexes can be created in one command.
    var multipleCreate = () => db.runCommand({
        createIndexes: coll.getName(),
        indexes: [
            {key: {a: 1}, name: "en_US", collation: {locale: "en_US"}},
            {key: {a: 1}, name: "en_US_1", collation: {locale: "en_US", strength: 1}}
        ]
    });
    assertIndexesCreated(multipleCreate, 2);

    // Cannot create another _id index.
    assert.commandFailed(coll.createIndex({_id: 1}, {name: "other", collation: {locale: "fr"}}));

    // Test that indexes must be dropped by name if the key pattern is ambiguous.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName()));

    // Create multiple indexes with the same key pattern and collation.
    assertIndexesCreated(() =>
                             coll.createIndex({a: 1}, {name: "foo", collation: {locale: "en_US"}}));
    assertIndexesCreated(
        () => coll.createIndex({a: 1}, {name: "bar", collation: {locale: "en_US", strength: 1}}));

    // Indexes cannot be dropped by an ambiguous key pattern.
    assert.commandFailed(coll.dropIndex({a: 1}));

    // Indexes can be dropped by name.
    assert.commandWorked(coll.dropIndex("foo"));
    assert.commandWorked(coll.dropIndex("bar"));

    // Test that hint behaves correctly in the presence of multiple indexes.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName()));

    assertIndexesCreated(() => coll.createIndex({a: 1}, {name: "sbc"}));
    assertIndexesCreated(
        () => coll.createIndex(
            {a: 1}, {name: "caseInsensitive", collation: {locale: "en_US", strength: 2}}));

    assert.writeOK(coll.insert([{a: "a"}, {a: "A"}, {a: 20}]));

    // An ambiguous hint pattern fails.
    assert.throws(() => coll.find({a: 1}).hint({a: 1}).itcount());
    if (db.getMongo().useReadCommands()) {
        assert.throws(
            () =>
                coll.find({a: 1}).collation({locale: "en_US", strength: 2}).hint({a: 1}).itcount());
    }

    // Index hint by name succeeds.
    assert.eq(coll.find({a: "a"}).hint("sbc").itcount(), 1);
    // A hint on an incompatible index does a whole index scan, and then filters using the query
    // collation.
    assert.eq(coll.find({a: "a"}).hint("caseInsensitive").itcount(), 1);
    if (db.getMongo().useReadCommands()) {
        assert.eq(
            coll.find({a: "a"}).collation({locale: "en_US", strength: 2}).hint("sbc").itcount(), 2);

        // A non-ambiguous index hint by key pattern is allowed, even if the collation doesn't
        // match.
        assertIndexesCreated(() => coll.createIndex({b: 1}, {collation: {locale: "fr"}}));
        assert.eq(coll.find({a: "a"}).collation({locale: "en_US"}).hint({b: 1}).itcount(), 1);
    }
})();
