'use strict';

/**
 * Performs concurrent upsert and delete operations against a small set of documents with a unique
 * index in place. One specific scenario this test exercises is upsert retry in the case where an
 * upsert generates an insert, which then fails due to another operation inserting first.
 */
var $config = (function() {
    const data = {
        numDocs: 4,
        getDocValue: function() {
            return Random.randInt(this.numDocs);
        },
    };

    const states = {
        delete: function(db, collName) {
            assertAlways.commandWorked(
                db[collName].remove({_id: this.getDocValue()}, {justOne: true}));
        },
        upsert: function(db, collName) {
            const value = this.getDocValue();
            const cmdRes = db.runCommand(
                {update: collName, updates: [{q: {_id: value}, u: {$inc: {y: 1}}, upsert: true}]});

            assertAlways.commandWorked(cmdRes);
        },
    };

    const transitions = {
        upsert: {upsert: 0.5, delete: 0.5},
        delete: {upsert: 0.5, delete: 0.5},
    };

    return {
        threadCount: 20,
        iterations: 100,
        states: states,
        startState: 'upsert',
        transitions: transitions,
        data: data,
    };
})();
