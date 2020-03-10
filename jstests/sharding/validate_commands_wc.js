/**
 * Tests whether mongos correctly validates write concerns.
 *
 * @tags: [requires_fcv_44]
 */
(function() {
'use strict';

var st = new ShardingTest({mongos: 1, shards: 1});
var mongos = st.s0;

var kDbName = 'db';

let db = mongos.getDB(kDbName);

assert.commandFailedWithCode(db.runCommand({ping: 1, writeConcern: {w: 1}}),
                             ErrorCodes.InvalidOptions);

assert.commandFailedWithCode(db.runCommand({ping: 1, writeConcern: {}}), ErrorCodes.InvalidOptions);

assert.commandWorked(db.runCommand({insert: "test", documents: [{_id: 1}], writeConcern: {w: 1}}));

assert.commandWorked(db.runCommand({insert: "test", documents: [{_id: 2}], writeConcern: {}}));

assert.commandWorked(
    db.runCommand({delete: "test", deletes: [{q: {_id: 1}, limit: 1}], writeConcern: {w: 1}}));

assert.commandWorked(
    db.runCommand({delete: "test", deletes: [{q: {_id: 2}, limit: 1}], writeConcern: {}}));

st.stop();
})();
