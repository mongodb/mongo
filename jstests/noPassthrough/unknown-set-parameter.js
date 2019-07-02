// Verify error is produced when specifying an invalid set parameter.

(function() {
    'use strict';

    function tryRun(arg) {
        // runMongoProgram helpfully makes certain that we pass a port when invoking mongod.
        return runMongoProgram('./mongod', '--port', 0, '--setParameter', arg, '--outputConfig');
    }

    // Positive case, valid setparam.
    clearRawMongoProgramOutput();
    const valid = tryRun('enableTestCommands=1');
    assert.eq(valid, 0);
    const validOutput = rawMongoProgramOutput();
    assert.gte(validOutput.search(/enableTestCommands: 1/), 0, validOutput);

    // Negative case, invalid setparam.
    clearRawMongoProgramOutput();
    const foo = tryRun('foo=bar');
    assert.neq(foo, 0);
    const fooOutput = rawMongoProgramOutput();
    assert.gte(fooOutput.search(/Unknown --setParameter 'foo'/), 0, fooOutput);

    // Negative case, valid but unavailable setparam.
    clearRawMongoProgramOutput();
    const graph = tryRun('roleGraphInvalidationIsFatal=true');
    assert.neq(graph, 0);
    const graphOutput = rawMongoProgramOutput();
    assert.gte(
        graphOutput.search(
            /--setParameter 'roleGraphInvalidationIsFatal' only available when used with 'enableTestCommands'/),
        0,
        fooOutput);

}());
