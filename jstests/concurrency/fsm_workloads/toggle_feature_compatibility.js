"use strict";

/**
 * toggle_feature_compatibility.js
 *
 * Adds and updates documents in some threads while rapidly toggling the feature
 * compatibility version between 3.4 and 3.6 in other threads, triggering the
 * failure in SERVER-30705.
 */
var $config = (function() {

    var states = (function() {

        function init(db, collName) {
        }

        function featureCompatibilityVersion34(db, collName) {
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
        }

        function featureCompatibilityVersion36(db, collName) {
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
        }

        function insertAndUpdate(db, collName) {
            let insertID = Random.randInt(1000000000);
            let res = db[collName].insert({_id: insertID});

            // Fail the test on any write error, except for a duplicate key error, which can
            // (rarely) happen when we accidentally choose the same random key more than once.
            assert(!res.hasWriteError() || res.getWriteError().code == ErrorCodes.DuplicateKey);
            assert.writeOK(db[collName].update({_id: insertID}, {$set: {b: 1, a: 1}}));
        }

        return {
            init: init,
            featureCompatibilityVersion34: featureCompatibilityVersion34,
            featureCompatibilityVersion36: featureCompatibilityVersion36,
            insertAndUpdate: insertAndUpdate
        };

    })();

    var transitions = {
        init: {featureCompatibilityVersion34: 0.5, insertAndUpdate: 0.5},
        featureCompatibilityVersion34: {featureCompatibilityVersion36: 1},
        featureCompatibilityVersion36: {featureCompatibilityVersion34: 1},
        insertAndUpdate: {insertAndUpdate: 1}
    };

    function teardown(db, collName, cluster) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
        assertWhenOwnColl(db[collName].drop());
    }

    return {
        threadCount: 8,
        iterations: 1000,
        data: null,
        states: states,
        transitions: transitions,
        teardown: teardown
    };
})();
