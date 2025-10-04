/**
 * Tests the --initializerShuffleSeed flag, which specifies the order that mongo initializers run
 * in.
 */

const regexMatch = /\{.+"id":4777800.+"nodes":\[(.+)\].+\}/;

const getInitializerOrderForSeed = (seed) => {
    clearRawMongoProgramOutput();
    const conn = MongoRunner.runMongod({verbose: 2, initializerShuffleSeed: seed});
    const logContents = rawMongoProgramOutput(".*");
    const match = logContents.match(regexMatch);
    assert(match.length > 0);
    MongoRunner.stopMongod(conn);

    return match[1];
};

// The same seed results in the same initializer order
assert.eq(getInitializerOrderForSeed(1234567), getInitializerOrderForSeed(1234567));

// Different seeds cause different initializer orders
assert.neq(getInitializerOrderForSeed(1234567), getInitializerOrderForSeed(891011));
