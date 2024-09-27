/**
 * Runs dbCheck in background.
 *
 * may need more checks, see:
 * jstests/concurrency/fsm_workloads/ddl/drop_database/drop_database_sharded_setFCV.js
 */

if (typeof db === 'undefined') {
    throw new Error(
        "Expected mongo shell to be connected a server, but global 'db' object isn't defined");
}

// Disable implicit sessions so FSM workloads that kill random sessions won't interrupt the
// operations in this test that aren't resilient to interruptions.
TestData.disableImplicitSessions = true;

const conn = db.getMongo();

const sendFCVUpDown = function(ver) {
    try {
        print("Running adminCommand({setFeatureCompatibilityVersion: " + ver + "}");
        const res = conn.adminCommand({setFeatureCompatibilityVersion: ver, confirm: true});
        assert.commandWorked(res);
    } catch (e) {
        if (e.code === 332) {
            // Cannot downgrade the cluster as collection xxx has 'encryptedFields' with range
            // indexes.
            jsTestLog('setFCV: Can not downgrade');
            return;
        }
        if (e.code === 5147403) {
            // Invalid fcv transition (e.g lastContinuous -> lastLTS).
            jsTestLog('setFCV: Invalid transition');
            return;
        }
        if (e.code === 7428200) {
            // Cannot upgrade FCV if a previous FCV downgrade stopped in the middle of cleaning
            // up internal server metadata.
            assert.eq(latestFCV, ver);
            jsTestLog(
                'setFCV: Cannot upgrade FCV if a previous FCV downgrade stopped in the middle \
				of cleaning up internal server metadata');
            return;
        }
        if (e.code === 12587) {
            // Cannot downgrade FCV that requires a collMod command when index builds are
            // concurrently taking place.
            jsTestLog(
                'setFCV: Cannot downgrade the FCV that requires a collMod command when index \
                builds are concurrently running');
            return;
        }
        if (e.code === ErrorCodes.MovePrimaryInProgress) {
            jsTestLog(
                'setFCV: Cannot downgrade the FCV that requires a collMod command when a move \
                Primary operation is running concurrently');
            return;
        }
        throw e;
    }
};

Random.setRandomSeed();
let maxSleep = 1000;  // 1 sec.
let currSleep = 10;   // Start at 10ms.

// Get time interval to sleep in ms.
// Value returned will be between currSleep and 2 * currSleep.
// Also increase currSleep in order to sleep for longer and longer intervals.
// This type of exponential backoff ensures that we run (several times) for short tests,
// but dont cause long tests to time out.
const getRandTimeIncInterval = function() {
    let ret = Random.randInt(currSleep) + currSleep;
    currSleep *= 4;
    return ret;
};

// Only go throug the loop a few times sleeping (by an increasing duration) between sendFCV
// commands. This is so even short duration tests can experience a few FCV changes, but for long
// running tests to not time out (which can happen if sleep duration was a fixed small duration).
while (currSleep <= maxSleep) {
    // downgrade FCV
    sleep(getRandTimeIncInterval());
    sendFCVUpDown(lastLTSFCV);

    // upgrade FCV
    sleep(getRandTimeIncInterval());
    sendFCVUpDown(latestFCV);
    // At this point FCV is back to latestFCV.

    if (lastLTSFCV !== lastContinuousFCV) {
        // downgrade FCV
        sleep(getRandTimeIncInterval());
        sendFCVUpDown(lastContinuousFCV);

        // upgrade FCV
        sleep(getRandTimeIncInterval());
        sendFCVUpDown(latestFCV);
        // At this point FCV is back to latestFCV.
    }
}
