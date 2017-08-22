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
    const count = coll.find({$jsonSchema: schema}).itcount();
    assert.eq(count, valid ? 1 : 0);
}
