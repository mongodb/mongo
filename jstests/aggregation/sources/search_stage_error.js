/**
 * Verify that the $search stage errors correctly if enterprise is not enabled.
 * @tags: [
 *   # $search/$searchMeta cannot be used within a facet
 *   do_not_wrap_aggregations_in_facets,
 *   # $search/$searchMeta do not support any read concern other than "local"
 *   assumes_read_concern_unchanged
 * ]
 */
(function() {
"use strict";

const coll = db.searchCollector;
coll.drop();
const buildInfo = assert.commandWorked(db.runCommand({"buildInfo": 1}));
if (buildInfo["modules"].includes("enterprise")) {
    // This is a test of behavior without enterprise.
    return;
}
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

// Check that a query with a $search stage errors without enterprise.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), cursor: {}, pipeline: [{$search: {}}]}), [6047401]);

// Check that a query with a $searchMeta stage errors without enterprise.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), cursor: {}, pipeline: [{$searchMeta: {}}]}),
    [6047401]);
})();
