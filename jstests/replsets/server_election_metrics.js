/**
 * Tests the format of electionMetrics serverStatus section.
 */
(function() {
    "use strict";

    // Verifies that the electionMetrics serverStatus section has the given field.
    function verifyElectionMetricsField(serverStatusResponse, fieldName) {
        assert(serverStatusResponse.electionMetrics.hasOwnProperty(fieldName),
               () => (`The 'electionMetrics' serverStatus section did not have the '${fieldName}' \
field: \n${tojson(serverStatusResponse.electionMetrics)}`));
        return serverStatusResponse.electionMetrics[fieldName];
    }

    // Verifies that the electionMetrics serverStatus section has a field for the given election
    // reason counter and that it has the subfields 'called' and 'successful'.
    function verifyElectionReasonCounterFields(serverStatusResponse, fieldName) {
        const field = verifyElectionMetricsField(serverStatusResponse, fieldName);
        assert(field.hasOwnProperty("called"),
               () => (`The '${fieldName}' field in the 'electionMetrics' serverStatus section did \
not have the 'called' field: \n${tojson(field)}`));
        assert(field.hasOwnProperty("successful"),
               () => (`The '${fieldName}' field in the 'electionMetrics' serverStatus section did \
not have the 'successful' field: \n${tojson(field)}`));
    }

    // Verifies the format of the electionMetrics serverStatus section.
    function verifyElectionMetricsSSS(serverStatusResponse) {
        assert(serverStatusResponse.hasOwnProperty("electionMetrics"),
               () => (`Expected the serverStatus response to have an 'electionMetrics' field:
${tojson(serverStatusResponse)}`));
        verifyElectionReasonCounterFields(serverStatusResponse, "stepUpCmd");
        verifyElectionReasonCounterFields(serverStatusResponse, "priorityTakeover");
        verifyElectionReasonCounterFields(serverStatusResponse, "catchUpTakeover");
        verifyElectionReasonCounterFields(serverStatusResponse, "electionTimeout");
        verifyElectionReasonCounterFields(serverStatusResponse, "freezeTimeout");
        verifyElectionMetricsField(serverStatusResponse, "numStepDownsCausedByHigherTerm");
        verifyElectionMetricsField(serverStatusResponse, "numCatchUps");
    }

    // Set up the replica set.
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const serverStatusResponse = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyElectionMetricsSSS(serverStatusResponse);

    // Stop the replica set.
    rst.stopSet();
}());
