/**
 * Test that renaming a collection to itself is not allowed
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_non_retryable_commands,
 * ]
 */

{
    // Rename a collection to itself fails without loosing data
    const sameColl = db['sameColl'];
    assert.commandWorked(sameColl.insert({a: 1}));

    const sameCollName = sameColl.toString().split('.')[1];

    let dropTarget = true;
    assert.commandFailedWithCode(sameColl.renameCollection(sameCollName, dropTarget),
                                 ErrorCodes.IllegalOperation);
    assert.eq(1, sameColl.countDocuments({}), "Rename a collection to itself must not lose data");

    sameColl.drop();  // Drop collection to avoid reusing it in case of repeated executions
}
