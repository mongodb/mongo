// Run a small set of tests using the ElectionTimingTest framework. While this
// reports the timing of the election, we are using it to check if any errors happen
// during different election cycles.
(function() {
    "use strict";
    load("jstests/libs/election_timing_test.js");
    var testStart = Date.now();

    var testCases = [
        {
          name: "testV1Stop",
          description: "protocolVersion 1, primary is stopped",
          protocolVersion: 1,
          // testRuns is the number of times a new ReplSetTest will be used.
          testRuns: 1,
          // testCycles is the number of election cycles that will be run per ReplSetTest lifespan.
          testCycles: 5,
          // testSetup is run after the replSet is initiated.
          // Function.prototype is the default.
          testSetup: Function.prototype,
          // Trigger an election by stepping down, stopping, or partitioning the primary.
          // stopPrimary is the default.
          electionTrigger: ElectionTimingTest.prototype.stopPrimary,
          // After the election has completed, make the old primary available again.
          // stopPrimaryReset is the default.
          testReset: ElectionTimingTest.prototype.stopPrimaryReset
        },

        /*
        This test case is flakey since waiting for the old primary to shutdown can take longer than
        the
        allowed timeout, even if a new primary was elected during the shutdown time.

        {
            name: "testV1StopTimeout1500",
            description: "protocolVersion 1, primary is stopped, electionTimeoutMillis set to 1500",
            protocolVersion: 1,
            testRuns: 1,
            testCycles: 5,
            // The settings object is merged into the replset config settings object.
            settings: {electionTimeoutMillis: 1500}
        },
        */

        {
          name: "testV1StepDown",
          description: "protocolVersion 1, primary is stepped down",
          protocolVersion: 1,
          testRuns: 1,
          testCycles: 5,
          electionTrigger: ElectionTimingTest.prototype.stepDownPrimary,
          testReset: ElectionTimingTest.prototype.stepDownPrimaryReset,
        },

        {
          name: "testV1StepDown1500",
          description: "protocolVersion 1, primary is stepped down",
          protocolVersion: 1,
          testRuns: 1,
          testCycles: 5,
          electionTrigger: ElectionTimingTest.prototype.stepDownPrimary,
          testReset: ElectionTimingTest.prototype.stepDownPrimaryReset,
          // The settings object is merged into the replset config settings object.
          settings: {electionTimeoutMillis: 1500}
        },

        {
          name: "testV1StepDownLargeCluster",
          description: "protocolVersion 1, primary is stepped down, 7 electable nodes",
          protocolVersion: 1,
          nodes: 7,
          testRuns: 1,
          testCycles: 5,
          electionTrigger: ElectionTimingTest.prototype.stepDownPrimary,
          testReset: function() {},
          waitForNewPrimary: function(rst, secondary) {
              rst.getPrimary();
          }
        },

        {
          name: "testV0Stop",
          description: "protocolVersion 0, primary is stopped",
          protocolVersion: 0,
          testRuns: 1,
          testCycles: 1
        },

        {
          name: "testV0StepDown",
          description: "protocolVersion 0, primary is stepped down",
          protocolVersion: 0,
          testRuns: 1,
          testCycles: 2,
          stepDownGuardTime: 30,
          // There is a guard time in pv0 that prevents an election right
          // after initiating.
          testSetup: function() {
              sleep(30 * 1000);
          },
          electionTrigger: ElectionTimingTest.prototype.stepDownPrimary,
          testReset: ElectionTimingTest.prototype.stepDownPrimaryReset
        },

    ];

    testCases.forEach(function(tc) {
        var testRun = new ElectionTimingTest(tc);
        tc.testResults = testRun.testResults;
        tc.electionTimeoutLimitMillis = testRun.electionTimeoutLimitMillis;

        if (testRun.testErrors.length) {
            // Stop tests if we encounter an error.
            // Dump available information for debugging.
            jsTestLog("Errors from: " + tc.name);
            printjson(tc);
            printjson(testRun.testErrors);
            throw new Error(testRun.testErrors[0].status);
        }
        // Print results of current test in case
        // we need to analyze a failed test later.
        jsTestLog("Raw Results: " + tc.name);
        printjson(tc.testResults);
    });

    testCases.forEach(function(tc) {
        var allResults = [];
        tc.testResults.forEach(function(tr) {
            allResults = allResults.concat(tr.results);
        });

        var resAvg = Array.avg(allResults);
        var resMin = Math.min(...allResults);
        var resMax = Math.max(...allResults);
        var resStdDev = Array.stdDev(allResults);

        jsTestLog("Results: " + tc.name + " Average over " + allResults.length + " runs: " +
                  resAvg + " Min: " + resMin + " Max: " + resMax + " Limit: " +
                  tc.electionTimeoutLimitMillis / 1000 + " StdDev: " + resStdDev.toFixed(4));

        allResults.forEach(function(failoverElapsedMillis) {
            assert.lte(failoverElapsedMillis,
                       tc.electionTimeoutLimitMillis / 1000,
                       tc.name + ': failover (' + failoverElapsedMillis +
                           ' sec) took too long. limit: ' + tc.electionTimeoutLimitMillis / 1000 +
                           ' sec');
        });
    });

    jsTestLog("Tests completed in: " + (Date.now() - testStart) / 1000 + " seconds");
}());
