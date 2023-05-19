
/**
 * Helper function to check a BulkWrite cursorEntry.
 */
const cursorEntryValidator = function(entry, expectedEntry) {
    assert.eq(entry.ok, expectedEntry.ok);
    assert.eq(entry.idx, expectedEntry.idx);
    assert.eq(entry.n, expectedEntry.n);
    assert.eq(entry.nModified, expectedEntry.nModified);
    assert.eq(entry.code, expectedEntry.code);
};
