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
    assert.eq(field["called"], value, `expected the 'called' field of ${fieldName} to be ${value}`);
    assert.eq(field["successful"],
              value,
              `expected the 'successful' field of ${fieldName} to be ${value}`);
}

/**
 * Verifies that the given election reason counter is incremented in the way we expect in the
 * 'electionMetrics' serverStatus section.
 */
function verifyServerStatusElectionReasonCounterChange(
    initialElectionMetrics, newElectionMetrics, fieldName, expectedIncrement) {
    const initialField = initialElectionMetrics[fieldName];
    const newField = newElectionMetrics[fieldName];
    assert.eq(initialField["called"] + expectedIncrement,
              newField["called"],
              `expected the 'called' field of ${fieldName} to increase by ${expectedIncrement}`);
    assert.eq(
        initialField["successful"] + expectedIncrement,
        newField["successful"],
        `expected the 'successful' field of ${fieldName} to increase by ${expectedIncrement}`);
}
