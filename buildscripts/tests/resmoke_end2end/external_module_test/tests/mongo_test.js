/**
 * Simple test that connects to MongoDB and performs basic operations.
 */

(function () {
    "use strict";

    const conn = db.getMongo();
    const testDB = conn.getDB("external_module_test");
    const coll = testDB.test_collection;

    // Drop collection to start fresh
    coll.drop();

    // Insert a document
    assert.commandWorked(coll.insert({_id: 1, name: "external_test", value: 42}));

    // Query the document
    const doc = coll.findOne({_id: 1});
    assert.eq(doc.name, "external_test", "Document name should match");
    assert.eq(doc.value, 42, "Document value should match");

    // Count documents
    assert.eq(coll.count(), 1, "Should have exactly one document");

    print("External module MongoDB test passed!");
})();
