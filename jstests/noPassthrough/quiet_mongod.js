(function() {
'use strict';

const regexMatch = /\"NETWORK.+51800.+client metadata\"/;

// Test that a normal mongod has client metadata.
{
    clearRawMongoProgramOutput();
    const conn = MongoRunner.runMongod();

    const mongoOutput = rawMongoProgramOutput();
    assert.gte(mongoOutput.match(regexMatch).length, 1);
    clearRawMongoProgramOutput();  // Clears output for next logging.

    MongoRunner.stopMongod(conn);
}

// Test that a quiet mongod does not output client metadata.
{
    clearRawMongoProgramOutput();
    const conn = MongoRunner.runMongod({quiet: ''});

    const mongoOutput = rawMongoProgramOutput();
    assert.eq(mongoOutput.match(regexMatch), null);
    clearRawMongoProgramOutput();  // Clears output for next logging.

    MongoRunner.stopMongod(conn);
}
})();
