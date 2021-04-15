/**
 * drop_collection_sharded.js
 *
 * Repeatedly creates and drops a collection.
 *
 * @tags: [
 *   requires_sharding,
 *   featureFlagShardingFullDDLSupport
 * ]
 */
'use strict';

var $config = (function() {
    var data = {
        collPrefix: 'sharded_coll_for_test_',
        collCount: 5,
    };

    var states = (function() {
        function init(db, collName) {
            this.collName = this.collPrefix + (this.tid % this.collCount);
        }

        function create(db, collName) {
            jsTestLog('Executing create state');
            const nss = db.getName() + '.' + this.collName;
            assertAlways.commandWorked(db.adminCommand({shardCollection: nss, key: {_id: 1}}));
        }

        function drop(db, collName) {
            jsTestLog('Executing drop state');
            assertAlways.commandWorked(db.runCommand({drop: this.collName}));
        }

        return {init: init, create: create, drop: drop};
    })();

    var transitions = {
        init: {create: 1},
        create: {create: 0.5, drop: 0.5},
        drop: {create: 0.5, drop: 0.5}
    };

    return {
        threadCount: 5,
        iterations: 50,
        startState: 'init',
        data: data,
        states: states,
        transitions: transitions
    };
})();