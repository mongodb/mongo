(function() {
'use strict';

const mongo = MongoRunner.runMongod();
const regexMatch = /sh([0-9]{1,10})\|/;

// Test that a normal mongo shell gives us some noise in the raw output.
{
    const out = runMongoProgram('mongo', '--port', mongo.port, '--eval', ';');
    const mongoOutput = rawMongoProgramOutput();

    assert.gte(mongoOutput.match(regexMatch).length, 1);
}

clearRawMongoProgramOutput();

// Test that a quiet shell does not output anything.
{
    const out = runMongoProgram('mongo', '--port', mongo.port, '--quiet', '--eval', ';');
    const mongoOutput = rawMongoProgramOutput();

    assert.eq(mongoOutput.match(regexMatch), null);
}

MongoRunner.stopMongod(mongo);
})();
