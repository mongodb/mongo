'use strict';

load('jstests/concurrency/fsm_libs/errors.js');  // for IterationEnd

/**
 * background_base.js
 *
 * This is a base background workload that provides two helpful states that can be
 * used in any derived workloads. It provides a 'wait' state that just waits a specified
 * number of milliseconds and a 'checkForTermination' state that checks to see if the
 * foreground workloads have finished. If they have, the state terminates the workload.
 */

var $config = (function() {

    var data = {
        millisecondsToSleep: 4000,
    };

    var states = {
        wait: function wait(db, collName) {
            sleep(this.millisecondsToSleep);
        },

        checkForTermination: function checkForTermination(db, collName) {
            var coll = db.getSiblingDB('config').fsm_background;
            var numDocs = coll.find({terminate: true}).itcount();
            if (numDocs >= 1) {
                throw new IterationEnd('Background workload was instructed to terminate');
            }
        }
    };

    var transitions = {wait: {checkForTermination: 1}, checkForTermination: {wait: 1}};

    var teardown = function teardown(db, collName, cluster) {
        db.getSiblingDB('config').fsm_background.drop();
    };

    return {
        threadCount: 1,
        iterations: Number.MAX_SAFE_INTEGER,
        data: data,
        states: states,
        startState: 'wait',
        teardown: teardown,
        transitions: transitions
    };
})();
