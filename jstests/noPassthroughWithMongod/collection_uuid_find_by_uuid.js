/**
 * Tests that when running the find command by UUID, the collectionUUID parameter cannot also be
 * specified.
 */
(function() {
'use strict';

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({_id: 0}));

const uuid = assert.commandWorked(db.runCommand({listCollections: 1}))
                 .cursor.firstBatch.find(c => c.name === coll.getName())
                 .info.uuid;

assert.commandFailedWithCode(db.runCommand({find: uuid, collectionUUID: uuid}),
                             ErrorCodes.InvalidOptions);
})();
