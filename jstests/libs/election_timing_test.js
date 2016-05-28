/**
 * ElectionTimingTest - set up a ReplSetTest and use default or provided functions to
 *  trigger an election. The time it takes to discover a new primary is recorded.
 */
var ElectionTimingTest = function(opts) {
    // How many times do we start a new ReplSetTest.
    this.testRuns = opts.testRuns || 1;

    // How many times do we step down during a ReplSetTest"s lifetime.
    this.testCycles = opts.testCycles || 1;

    // The config is set to two electable nodes since we use waitForMemberState
    // to wait for the electable secondary to become primary.
    this.nodes = opts.nodes || [{}, {}, {rsConfig: {arbiterOnly: true}}];

    // The name of the replica set and of the collection.
    this.name = opts.name || "election_timing";

    // Pass additional replicaSet config options.
    this.settings = opts.settings || {};

    // pv1 is the default in master and here.
    this.protocolVersion = opts.hasOwnProperty("protocolVersion") ? opts.protocolVersion : 1;

    // A function that runs after the ReplSetTest is initialized.
    this.testSetup = opts.testSetup || Function.prototype;

    // A function that triggers election, default is to kill the mongod process.
    this.electionTrigger = opts.electionTrigger || this.stopPrimary;

    // A function that waits for new primary to be elected.
    this.waitForNewPrimary = opts.waitForNewPrimary || this.waitForNewPrimary;

    // A function that cleans up after the election trigger.
    this.testReset = opts.testReset || this.stopPrimaryReset;

    // The interval passed to stepdown that primaries may not seek re-election.
    // We also have to wait out this interval before allowing another stepdown.
    this.stepDownGuardTime = opts.stepDownGuardTime || 60;

    // Test results will be stored in these arrays.
    this.testResults = [];
    this.testErrors = [];

    this._runTimingTest();
};

ElectionTimingTest.prototype._runTimingTest = function() {
    for (var run = 0; run < this.testRuns; run++) {
        var collectionName = "test." + this.name;
        var cycleData = {testRun: run, results: []};

        jsTestLog("Starting ReplSetTest for test " + this.name + " run: " + run);
        this.rst =
            new ReplSetTest({name: this.name, nodes: this.nodes, nodeOptions: {verbose: ""}});
        this.rst.startSet();

        // Get the replset config and apply the settings object.
        var conf = this.rst.getReplSetConfig();
        conf.settings = conf.settings || {};
        conf.settings = Object.merge(conf.settings, this.settings);

        // Explicitly setting protocolVersion.
        conf.protocolVersion = this.protocolVersion;
        this.rst.initiate(conf);

        // Run the user supplied testSetup() method. Typical uses would be to set up
        // bridging, or wait for a particular state after initiate().
        try {
            this.testSetup();
        } catch (e) {
            // If testSetup() fails, we are in an unknown state, log and return.
            this.testErrors.push({testRun: run, status: "testSetup() failed", error: e});
            this.rst.stopSet();
            return;
        }

        // Create and populate a collection.
        var primary = this.rst.getPrimary();

        this.electionTimeoutLimitMillis =
            ElectionTimingTest.calculateElectionTimeoutLimitMillis(primary);
        jsTestLog('Election timeout limit: ' + this.electionTimeoutLimitMillis + ' ms');

        var coll = primary.getCollection(collectionName);
        for (var i = 0; i < 100; i++) {
            assert.writeOK(coll.insert({_id: i, x: i * 3, arbitraryStr: "this is a string"}));
        }

        // Run the election tests on this ReplSetTest instance.
        var secondary;
        for (var cycle = 0; cycle < this.testCycles; cycle++) {
            // Wait for replication.
            this.rst.awaitSecondaryNodes();
            this.rst.awaitReplication();
            primary = this.rst.getPrimary();
            secondary = this.rst.getSecondary();

            jsTestLog("Starting test: " + this.name + " run: " + run + " cycle: " + cycle);
            var isMasterResult = primary.getDB("admin").isMaster();
            assert.commandWorked(isMasterResult, "isMaster() failed");
            var oldElectionId = isMasterResult.electionId;
            assert.neq(undefined, oldElectionId, "isMaster() failed to return a valid electionId");

            // Time the new election.
            var stepDownTime = Date.now();

            // Run the specified election trigger method. Default is to sigstop the primary.
            try {
                this.electionTrigger();
            } catch (e) {
                // Left empty on purpose.
            }

            // Wait for the electable secondary to become primary.
            try {
                this.waitForNewPrimary(this.rst, secondary);
            } catch (e) {
                // If we didn"t find a primary, save the error, break so this
                // ReplSetTest is stopped. We can"t continue from a flaky state.
                this.testErrors.push(
                    {testRun: run, cycle: cycle, status: "new primary not elected", error: e});
                break;
            }

            var electionCompleteTime = Date.now();

            // Verify we had an election and we have a new primary.
            var newPrimary = this.rst.getPrimary();
            isMasterResult = newPrimary.getDB("admin").isMaster();
            assert.commandWorked(isMasterResult, "isMaster() failed");
            var newElectionId = isMasterResult.electionId;
            assert.neq(undefined, newElectionId, "isMaster() failed to return a valid electionId");

            if (bsonWoCompare(oldElectionId, newElectionId) !== 0) {
                this.testErrors.push({
                    testRun: run,
                    cycle: cycle,
                    status: "electionId not changed, no election was triggered"
                });
                break;
            }

            if (primary.host === newPrimary.host) {
                this.testErrors.push(
                    {testRun: run, cycle: cycle, status: "Previous primary was re-elected"});
                break;
            }

            cycleData.results.push((electionCompleteTime - stepDownTime) / 1000);

            // If we are running another test on this ReplSetTest, call the reset function.
            if (cycle + 1 < this.testCycles) {
                try {
                    this.testReset();
                } catch (e) {
                    this.testErrors.push(
                        {testRun: run, cycle: cycle, status: "testReset() failed", error: e});
                    break;
                }
            }
        }
        this.testResults.push(cycleData);
        this.rst.stopSet();
    }
};

ElectionTimingTest.prototype.stopPrimary = function() {
    this.originalPrimary = this.rst.getNodeId(this.rst.getPrimary());
    this.rst.stop(this.originalPrimary);
};

ElectionTimingTest.prototype.stopPrimaryReset = function() {
    this.rst.restart(this.originalPrimary);
};

ElectionTimingTest.prototype.stepDownPrimary = function() {
    var adminDB = this.rst.getPrimary().getDB("admin");
    adminDB.runCommand({replSetStepDown: this.stepDownGuardTime, force: true});
};

ElectionTimingTest.prototype.stepDownPrimaryReset = function() {
    sleep(this.stepDownGuardTime * 1000);
};

ElectionTimingTest.prototype.waitForNewPrimary = function(rst, secondary) {
    assert.commandWorked(secondary.adminCommand({
        replSetTest: 1,
        waitForMemberState: ReplSetTest.State.PRIMARY,
        timeoutMillis: 60 * 1000
    }),
                         "node " + secondary.host + " failed to become primary");
};

/**
 * Calculates upper limit for actual failover time in milliseconds.
 */
ElectionTimingTest.calculateElectionTimeoutLimitMillis = function(primary) {
    var configResult = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1}));
    var config = configResult.config;
    // Protocol version is 0 if missing from config.
    var protocolVersion = config.hasOwnProperty("protocolVersion") ? config.protocolVersion : 0;
    var electionTimeoutMillis = 0;
    var electionTimeoutOffsetLimitFraction = 0;
    if (protocolVersion === 0) {
        electionTimeoutMillis = 30000;  // from TopologyCoordinatorImpl::VoteLease::leaseTime
        electionTimeoutOffsetLimitFraction = 0;
    } else {
        electionTimeoutMillis = config.settings.electionTimeoutMillis;
        var getParameterResult = assert.commandWorked(primary.adminCommand({
            getParameter: 1,
            replElectionTimeoutOffsetLimitFraction: 1,
        }));
        electionTimeoutOffsetLimitFraction =
            getParameterResult.replElectionTimeoutOffsetLimitFraction;
    }
    var assertSoonIntervalMillis = 200;  // from assert.js
    var applierDrainWaitMillis = 1000;   // from SyncTail::tryPopAndWaitForMore()
    var electionTimeoutLimitMillis =
        (1 + electionTimeoutOffsetLimitFraction) * electionTimeoutMillis + applierDrainWaitMillis +
        assertSoonIntervalMillis;
    return electionTimeoutLimitMillis;
};
