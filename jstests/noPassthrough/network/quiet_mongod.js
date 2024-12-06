/**
 * Verifies that client metadata is not logged when in quiet mode, and is logged otherwise.
 */

(function() {
'use strict';

const dbName = jsTestName();
const firstCommandLogLineRegexS = "\"NETWORK.+6788700.+Received first command";
const firstCommandLogLineRegex = new RegExp(firstCommandLogLineRegexS);
const clientMetadataLogLineRegexS = "\"NETWORK.+51800.+client metadata\"";
const clientMetadataLogLineRegex = new RegExp(clientMetadataLogLineRegexS);

// Test that a normal mongod has client metadata.
{
    const conn = MongoRunner.runMongod();

    // Issue a command and wait for a log line acknowledging it to be sure any client metadata log
    // lines have been flushed.
    conn.getDB(dbName).runCommand({ping: 1});
    assert.soon(
        () => rawMongoProgramOutput(firstCommandLogLineRegexS).match(firstCommandLogLineRegex),
        "did not see log line acknowledging first command");

    const mongoOutput = rawMongoProgramOutput(clientMetadataLogLineRegexS);
    assert.gte(mongoOutput.match(clientMetadataLogLineRegex).length,
               1,
               "did not see client metadata log line while not in quiet mode");

    MongoRunner.stopMongod(conn);
    clearRawMongoProgramOutput();  // Clears output for next logging.
}

// Test that a quiet mongod does not output client metadata.
{
    const conn = MongoRunner.runMongod({quiet: ''});

    // Issue a command and wait for a log line acknowledging it to be sure any client metadata log
    // lines have been flushed.
    conn.getDB(dbName).runCommand({ping: 1});
    assert.soon(
        () => rawMongoProgramOutput(firstCommandLogLineRegexS).match(firstCommandLogLineRegex),
        "did not see log line acknowledging first command");

    const mongoOutput = rawMongoProgramOutput(clientMetadataLogLineRegexS);
    assert.eq(mongoOutput.match(clientMetadataLogLineRegex),
              null,
              "saw client metadata log line while in quiet mode");

    MongoRunner.stopMongod(conn);
    clearRawMongoProgramOutput();  // Clears output for next logging.
}
})();
