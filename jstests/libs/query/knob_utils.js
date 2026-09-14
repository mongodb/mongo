/*
 * Helpers for running a function with query knobs and/or failpoints temporarily set.
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function runSetParamCommand(adminDb, knobToVal) {
    FixtureHelpers.runCommandOnAllShards({db: adminDb, cmdObj: {setParameter: 1, ...knobToVal}});
}

function runSetFailpointCommand(adminDb, fpName, fpMode) {
    FixtureHelpers.runCommandOnAllShards({
        db: adminDb,
        cmdObj: {configureFailPoint: fpName, mode: fpMode},
    });
}

/*
 * Runs the given function with the query knobs and/or failpoints set, then sets the values
 * back to their original state before exiting.
 * It's important that each run of the property is independent from one another, so we'll always
 * reset the knobs to their original state even if the function throws an exception.
 */
export function runWithKnobs(db, fn, knobToVal = {}, failPointToMode = {}) {
    const adminDb = db.getSiblingDB("admin");
    const knobNames = Object.keys(knobToVal);
    const failPointNames = Object.keys(failPointToMode);
    // If there are no knobs or failpoints to change, return the result of the function since
    // there's no other work to do.
    if (knobNames.length === 0 && failPointNames.length === 0) {
        return fn();
    }

    // Get the previous knob settings, so we can undo our changes after setting the knobs from
    // `knobToVal`.
    const priorKnobSettings = {};
    if (knobNames.length > 0) {
        const getParamObj = {getParameter: 1};
        for (const key of knobNames) {
            getParamObj[key] = 1;
        }
        const getParamResult = assert.commandWorked(adminDb.adminCommand(getParamObj));
        for (const key of knobNames) {
            priorKnobSettings[key] = getParamResult[key];
        }

        // Set the requested knobs.
        runSetParamCommand(adminDb, knobToVal);
    }

    // Capture the prior failpoint modes so we can restore them, then turn on the requested
    // failpoints.
    const priorFailPointModes = {};
    for (const fpName of failPointNames) {
        const paramName = `failpoint.${fpName}`;
        const getParamResult = assert.commandWorked(
            adminDb.adminCommand({getParameter: 1, [paramName]: 1}),
        );
        // Mode is a 1 or 0 for on or off failpoint status.
        const priorModeStr = getParamResult[paramName].mode ? "alwaysOn" : "off";
        priorFailPointModes[fpName] = priorModeStr;

        runSetFailpointCommand(adminDb, fpName, failPointToMode[fpName]);
    }

    // With the finally block, we'll always revert the parameters back to their original settings,
    // even if an exception is thrown.
    try {
        return fn();
    } finally {
        // Reset to the original settings.
        if (knobNames.length > 0) {
            runSetParamCommand(adminDb, priorKnobSettings);
        }
        for (const fpName of failPointNames) {
            runSetFailpointCommand(adminDb, fpName, priorFailPointModes[fpName]);
        }
    }
}
