/**
 * If 'valid' is true, asserts that 'doc' matches 'schema', by wrapping 'schema' in a $jsonSchema
 * predicate and issuing a query against collection 'coll'. Drops 'coll' in the process, so do not
 * pass a collection whose contents you wish to preserve.
 *
 * If valid is false, asserts that 'doc' does not match 'schema'.
 */
function assertSchemaMatch(coll, schema, doc, valid) {
    coll.drop();
    assert.writeOK(coll.insert(doc));
    let count = coll.find({$jsonSchema: schema}).itcount();
    const errmsg = "Document " + tojson(doc) +
        (valid ? " should have matched the schema " : " unexpectedly matched the schema ") +
        tojson(schema);
    assert.eq(count, valid ? 1 : 0, errmsg);

    // Repeat the same query in an aggregation $match stage.
    count = coll.aggregate([{$match: {$jsonSchema: schema}}]).itcount();
    assert.eq(count, valid ? 1 : 0, errmsg + " in a $match stage");
}
