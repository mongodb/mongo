// Test for stripping whitespace for authenticationMechanisms
(function() {
"use strict";

const conn = MongoRunner.runMongod(
    {setParameter: "authenticationMechanisms=SCRAM-SHA-1,SCRAM-SHA-256, PLAIN"});

const cmdOut = conn.getDB('admin').runCommand({getParameter: 1, authenticationMechanisms: 1});

// Check to see if whitespace in front of PLAIN is stripped
assert.sameMembers(cmdOut.authenticationMechanisms, ["SCRAM-SHA-1", "SCRAM-SHA-256", "PLAIN"]);
MongoRunner.stopMongod(conn);
}());
