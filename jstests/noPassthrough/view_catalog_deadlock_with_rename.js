/**
 * This test demonstrates the deadlock we found in SERVER-40994: Rename locks
 * the source and target based on their resourceId order, and a delete op on an
 * non-existent collection (which also triggers a view catalog reload) locks the
 * same two namespaces in different order.
 *
 * The fix is to always lock 'system.views' collection in the end.
 */
(function() {
    'use strict';

    const conn = MongoRunner.runMongod();
    const db = conn.getDB('test');

    assert.commandWorked(db.runCommand({insert: 'a', documents: [{x: 1}]}));
    assert.commandWorked(db.runCommand({insert: 'b', documents: [{x: 1}]}));

    assert.commandWorked(db.createView('viewA', 'a', []));

    // Will cause a view catalog reload.
    assert.commandWorked(db.runCommand(
        {insert: 'system.views', documents: [{_id: 'test.viewB', viewOn: 'b', pipeline: []}]}));

    const renameSystemViews = startParallelShell(function() {
        // This used to first lock 'test.system.views' and then 'test.aaabb' in X mode.
        assert.commandWorked(
            db.adminCommand({renameCollection: 'test.system.views', to: 'test.aaabb'}));
    }, conn.port);

    // This triggers view catalog reload. Therefore it first locked 'test.aaabb' in IX mode and then
    // 'test.system.views' in IS mode.
    assert.commandWorked(db.runCommand({delete: 'aaabb', deletes: [{q: {x: 2}, limit: 1}]}));

    renameSystemViews();
    MongoRunner.stopMongod(conn);
})();
