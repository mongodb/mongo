'use strict';

/**
 * profile_command.js
 *
 * Sets the profile level and filter to different values, and asserts that they take effect
 * simultaneously:
 *   1. The profile command never returns an inconsistent combination of {level, filter}.
 *   2. We never log or profile based on an inconsistent combination of {level, filter}.
 *
 * @tags: [requires_profiling, requires_fcv_47]
 */

var $config = (function() {
    const data = {
        numDocs: 1000,
        checkProfileResult: function(result) {
            // After init(), these are the only profile settings we set.
            // For example, we never set { level: 1, filter {millis: {$gt: 1}} }.
            assert.contains({level: result.was, filter: result.filter}, [
                {level: 0, filter: {$expr: {$const: false}}},
                {level: 0, filter: {nreturned: {$gt: 0}}},
                {level: 1, filter: {nModified: {$gt: 0}}},
            ]);
        },
    };
    const states = {
        init: function(db, collName) {},

        logReads: function(db, collName) {
            const result = db.setProfilingLevel(0, {filter: {nreturned: {$gt: 0}}});
            this.checkProfileResult(result);
        },
        profileUpdates: function(db, collName) {
            const result = db.setProfilingLevel(1, {filter: {nModified: {$gt: 0}}});
            this.checkProfileResult(result);
        },

        readSomething: function(db, collName) {
            assert.eq(this.numDocs, db[collName].count());
            const numDocs = Random.randInt(this.numDocs);
            const n = db[collName].find({_id: {$lt: numDocs}}).comment('readSomething').itcount();
            assert.eq(n, numDocs);
        },
        updateSomething: function(db, collName) {
            assert.eq(this.numDocs, db[collName].count());
            const numDocs = Random.randInt(this.numDocs);
            const resp = assert.commandWorked(db[collName].updateMany(
                {_id: {$lt: numDocs}}, {$inc: {i: 1}}, {comment: 'updateSomething'}));
            assert.eq(numDocs, resp.matchedCount);
            assert.eq(numDocs, resp.modifiedCount);
        },

    };
    const setup = function(db, collName) {
        Random.setRandomSeed();
        db.setProfilingLevel(0, {filter: {$expr: false}});
        const docs = Array.from({length: this.numDocs}, (_, i) => ({_id: i}));
        assert.commandWorked(db[collName].insert(docs));
    };
    const teardown = function(db, collName) {
        db.setProfilingLevel(0, {filter: 'unset'});
        // We never profiled a read.
        assert.eq(0, db.system.profile.find({comment: 'readSomething'}).itcount());
    };

    // Just let every state go to every other state with equal probability.
    const weights = {
        logReads: 0.25,
        profileUpdates: 0.25,
        readSomething: 0.25,
        updateSomething: 0.25,
    };
    const transitions = {
        init: weights,
        logReads: weights,
        profileUpdates: weights,
        readSomething: weights,
        updateSomething: weights,
    };
    return {
        data,
        threadCount: 4,
        iterations: 100,
        states,
        transitions,
        setup,
        teardown,
    };
})();
