/**
 * Tests that the write error messages' strings are truncated properly.
 *
 * @tags: [
 *   assumes_write_concern_unchanged,
 * ]
 */

(function() {
'use strict';

var coll = db.write_error_message_truncation;
coll.drop();

// Insert 3 documents that we will then reinsert in order to generate DuplicateKey errors
assert.writeOK(coll.insert(
    [
        {_id: 0},
        {_id: 1},
        {_id: 2},
    ],
    {writeConcern: {w: 'majority'}, ordered: true}));

// Ensure DuplicateKey errors during insert all report their messages
var res = coll.insert(
    [
        {_id: 0},
        {_id: 1},
        {_id: 2},
    ],
    {writeConcern: {w: 'majority'}, ordered: false});

jsTest.log(res.getRawResponse());

assert.eq(0, res.nInserted);

var writeErrors = res.getWriteErrors();
assert.eq(3, writeErrors.length);
assert.neq('', writeErrors[0].errmsg);
assert.neq('', writeErrors[1].errmsg);
assert.neq('', writeErrors[2].errmsg);
})();
