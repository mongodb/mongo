/**
 * Wraps 'schema' in a $jsonSchema predicate and asserts the validity of 'doc' against schema via a
 * variety of different methods:
 *
 *  - find command
 *  - aggregation command via $match
 *  - insert command with a $jsonSchema validator
 *  - update command with a $jsonSchema validator (if possible)
 *
 * Asserts that 'doc' matches 'schema' if and only if 'valid' is true. Drops 'coll' in the process,
 * so do not pass a collection whose contents you wish to preserve.
 */
function assertSchemaMatch(coll, schema, doc, valid) {
    const errmsg = "Document " + tojson(doc) +
        (valid ? " should have matched the schema " : " unexpectedly matched the schema ") +
        tojson(schema);

    // Test that after inserting 'doc', we can find it again using $jsonSchema in the find command
    // iff 'valid' is true.
    coll.drop();
    assert.writeOK(coll.insert(doc));
    let count = coll.find({$jsonSchema: schema}).itcount();
    assert.eq(count, valid ? 1 : 0, errmsg);

    // Test that after inserting 'doc', we can find it again using $jsonSchema in an aggregation
    // $match stage iff 'valid' is true.
    count = coll.aggregate([{$match: {$jsonSchema: schema}}]).itcount();
    assert.eq(count, valid ? 1 : 0, errmsg + " in a $match stage");

    // Test that 'doc' can be inserted into a collection using 'schema' as its document validator
    // iff 'valid' is true. We explicitly use runCommand to issue the drop to avoid an implicit
    // collection creation in sharded_collections_jscore_passthrough.
    assert.commandWorked(coll.runCommand("drop"));
    assert.commandWorked(coll.runCommand("create", {validator: {$jsonSchema: schema}}));
    let res = coll.insert(doc);
    if (valid) {
        assert.writeOK(res, errmsg + " during insert document validation");
    } else {
        assert.writeErrorWithCode(res,
                                  ErrorCodes.DocumentFailedValidation,
                                  errmsg + " during insert document validation");
    }

    // Test that we can update an existing document to look like 'doc' when the collection has
    // 'schema' as its document validator in "strict" mode iff 'valid' is true.
    assert.commandWorked(coll.runCommand("drop"));
    assert.writeOK(coll.insert({_id: 0}));
    assert.commandWorked(
        coll.runCommand("collMod", {validator: {$jsonSchema: schema}, validationLevel: "strict"}));

    // Before applying the update, remove the _id field if it exists, or the replacement-style
    // update will fail.
    let docCopy = Object.extend({}, doc);
    delete docCopy._id;
    res = coll.update({_id: 0}, docCopy);
    if (valid) {
        assert.writeOK(res, errmsg + " during update document validation in strict mode");
    } else {
        assert.writeErrorWithCode(res,
                                  ErrorCodes.DocumentFailedValidation,
                                  errmsg + " during update document validation in strict mode");
    }
}
