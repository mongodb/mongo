/**
 * Verify that the $search stage errors correctly if a mongot host is not connected or configured.
 * @tags: [
 *   # $search/$searchMeta cannot be used within a facet
 *   do_not_wrap_aggregations_in_facets,
 *   # $search/$searchMeta do not support any read concern other than "local"
 *   assumes_read_concern_unchanged,
 *   not_allowed_with_signed_security_token,
 * ]
 */
const coll = db.searchCollector;
coll.drop();
const buildInfo = assert.commandWorked(db.runCommand({"buildInfo": 1}));

assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

// Check that a query with a $search stage errors without mongot.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), cursor: {}, pipeline: [{$search: {}}]}),
    [ErrorCodes.SearchNotEnabled]);

// Check that a query with a $searchMeta stage errors without mongot.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), cursor: {}, pipeline: [{$searchMeta: {}}]}),
    [ErrorCodes.SearchNotEnabled]);

// Check that a query with a $listSearchIndexes stage errors without mongot.
assert.commandFailedWithCode(
    coll.runCommand({aggregate: coll.getName(), pipeline: [{$listSearchIndexes: {}}], cursor: {}}),
    [ErrorCodes.SearchNotEnabled]);

// Check that a query with a $vectorSearch stage errors without mongot.
assert.commandFailedWithCode(
    coll.runCommand(
        {aggregate: coll.getName(), cursor: {}, pipeline: [{$vectorSearch: {limit: 100}}]}),
    [ErrorCodes.SearchNotEnabled]);
