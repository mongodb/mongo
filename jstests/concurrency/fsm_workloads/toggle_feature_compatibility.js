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
            this.isInFeatureCompatibilityVersion36 = true;
        }

        function flipFeatureCompatibilityVersion(db, data) {
            data.isInFeatureCompatibilityVersion36 = !data.isInFeatureCompatibilityVersion36;
            const newFeatureCompatibilityVersion =
                data.isInFeatureCompatibilityVersion36 ? "3.6" : "3.4";
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: newFeatureCompatibilityVersion}));
        }

        function insertAndUpdate(db, collName) {
            // The desired behavior is to have one thread constantly executing
            // flipFeatureCompatibilityVersion() and all the other threads
            // constantly executing insertAndUpdate(). Since we can't explicitly
            // set the state for each thread, we just have one transition
            // function that changes behavior based on which thread it is in.
            if (this.tid == 0) {
                flipFeatureCompatibilityVersion(db, this);
                return;
            }

            let insertID = Random.randInt(1000000000);
            let res = db[collName].insert({_id: insertID});

            // Fail the test on any write error, except for a duplicate key error, which can
            // (rarely) happen when we accidentally choose the same random key more than once.
            assert(!res.hasWriteError() || res.getWriteError().code == ErrorCodes.DuplicateKey,
                   "Failed insert: " + tojson(res));
            assert.writeOK(db[collName].update({_id: insertID}, {$set: {b: 1, a: 1}}));
        }

        return {init: init, insertAndUpdate: insertAndUpdate};

    })();

    var transitions = {init: {insertAndUpdate: 1}, insertAndUpdate: {insertAndUpdate: 1}};

    function teardown(db, collName, cluster) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    }

    return {
        threadCount: 8,
        iterations: 300,
        data: null,
        states: states,
        transitions: transitions,
        teardown: teardown
    };
})();
