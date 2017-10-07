/**
 * Test that a $changeStream pipeline adopts either the user-specified collation, or the default of
 * the target collection if no specific collation is requested.
 * TODO SERVER-31443: Update these tests to verify full collation support with $changeStream.
 */
(function() {
    "use strict";

    const noCollationColl = db.change_stream_no_collation;
    const hasCollationColl = db.change_stream_collation;

    hasCollationColl.drop();
    noCollationColl.drop();

    assert.commandWorked(
        db.runCommand({create: hasCollationColl.getName(), collation: {locale: "en_US"}}));
    assert.commandWorked(db.runCommand({create: noCollationColl.getName()}));

    assert.writeOK(hasCollationColl.insert({_id: 1}));
    assert.writeOK(noCollationColl.insert({_id: 1}));

    const csPipeline = [{$changeStream: {}}];
    const simpleCollation = {collation: {locale: "simple"}};
    const nonSimpleCollation = {collation: {locale: "en_US"}};

    // Verify that we can open a $changeStream on a collection whose default collation is 'simple'
    // without specifying a collation in our request.
    let csCursor = assert.doesNotThrow(() => noCollationColl.aggregate(csPipeline));
    csCursor.close();

    // Verify that we cannot open a $changeStream if we specify a non-simple collation.
    let csError = assert.throws(() => noCollationColl.aggregate(csPipeline, nonSimpleCollation));
    assert.eq(csError.code, 40471);

    // Verify that we cannot open a $changeStream on a collection with a non-simple default
    // collation if we omit a collation specification in the request.
    csError = assert.throws(() => hasCollationColl.aggregate(csPipeline));
    assert.eq(csError.code, 40471);

    // Verify that we can open a $changeStream on a collection with a non-simple default collation
    // if we explicitly request a 'simple' collator.
    csCursor = assert.doesNotThrow(() => hasCollationColl.aggregate(csPipeline, simpleCollation));
    csCursor.close();
})();
