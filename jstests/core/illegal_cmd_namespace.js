/**
 * Test that an attempt to run a query over a $cmd namespace is not treated specially by the shell,
 * but is rejected by the server.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";

function testBadNamespace(collName) {
    const coll = db[collName];
    assert.commandFailedWithCode(db.runCommand({find: collName}), ErrorCodes.InvalidNamespace);
    assert.throwsWithCode(() => coll.find().itcount(), ErrorCodes.InvalidNamespace);
    assert.throwsWithCode(() => coll.findOne(), ErrorCodes.InvalidNamespace);
}

testBadNamespace("$cmd");
testBadNamespace("$cmd.foo");

// These namespaces were formerly accepted by old versions of the server as so-called
// "pseudo-commands".
testBadNamespace("$cmd.sys.inprog");
testBadNamespace("$cmd.sys.killop");
testBadNamespace("$cmd.sys.unlock");

// These namespaces are used internally, but queries over them should be rejected.
testBadNamespace("$cmd.listCollections");
testBadNamespace("$cmd.aggregate");

// "$cmd" or "$" are not allowed in the collection name in general.
testBadNamespace("a$cmdb");
testBadNamespace("$");
testBadNamespace("a$b");
}());
