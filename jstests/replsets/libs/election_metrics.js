/**
 * Contains helper functions for testing the values of fields in the 'electionMetrics' serverStatus
 * section.
 */

/**
 * Verifies that the given election reason counter has the value we expect in the
 * 'electionMetrics' serverStatus section.
 */
function verifyServerStatusElectionReasonCounterValue(electionMetrics, fieldName, value) {
    const field = electionMetrics[fieldName];
    assert.eq(
        field["called"], value, `expected the 'called' field of '${fieldName}' to be ${value}`);
    assert.eq(field["successful"],
              value,
              `expected the 'successful' field of '${fieldName}' to be ${value}`);
}

/**
 * Verifies that the given election reason counter is incremented in the way we expect in the
 * 'electionMetrics' serverStatus section.
 *
 * The 'expectedNumSuccessful' field should be passed in when we need to distinguish between how
 * many times an election was called and how many times an election was successful.
 */
function verifyServerStatusElectionReasonCounterChange(initialElectionMetrics,
                                                       newElectionMetrics,
                                                       fieldName,
                                                       expectedNumCalled,
                                                       expectedNumSuccessful = undefined,
                                                       allowGreater = false) {
    // If 'expectedNumSuccessful' is not passed in, we assume that the 'successful' field is equal
    // to the 'called' field.
    if (!expectedNumSuccessful) {
        expectedNumSuccessful = expectedNumCalled;
    }

    const initialField = initialElectionMetrics[fieldName];
    const newField = newElectionMetrics[fieldName];

    const assertFunc = function(left, right, msg) {
        if (allowGreater) {
            assert.gte(left, right, msg);
        } else {
            assert.eq(left, right, msg);
        }
    };

    assertFunc(initialField["called"] + expectedNumCalled,
               newField["called"],
               `expected the 'called' field of '${fieldName}' to increase by ${expectedNumCalled}`);
    assertFunc(initialField["successful"] + expectedNumSuccessful,
               newField["successful"],
               `expected the 'successful' field of '${fieldName}' to increase by ${
                   expectedNumSuccessful}`);
}

/**
 * Verifies that the given field in serverStatus is incremented in the way we expect.
 */
function verifyServerStatusChange(initialStatus, newStatus, fieldName, expectedIncrement) {
    assert.eq(initialStatus[fieldName] + expectedIncrement,
              newStatus[fieldName],
              `expected '${fieldName}' to increase by ${expectedIncrement}`);
}

/**
 * Verifies that the given reason for primary catchup concluding is incremented in serverStatus, and
 * that none of the other reasons are.
 */
function verifyCatchUpConclusionReason(initialStatus, newStatus, fieldName) {
    const catchUpConclusionMetrics = [
        "numCatchUpsSucceeded",
        "numCatchUpsAlreadyCaughtUp",
        "numCatchUpsSkipped",
        "numCatchUpsTimedOut",
        "numCatchUpsFailedWithError",
        "numCatchUpsFailedWithNewTerm",
        "numCatchUpsFailedWithReplSetAbortPrimaryCatchUpCmd"
    ];

    catchUpConclusionMetrics.forEach(function(metric) {
        if (metric === fieldName) {
            assert.eq(initialStatus[metric] + 1,
                      newStatus[metric],
                      `expected '${metric}' to increase by 1`);
        } else {
            assert.eq(
                initialStatus[metric], newStatus[metric], `did not expect '${metric}' to increase`);
        }
    });
}
