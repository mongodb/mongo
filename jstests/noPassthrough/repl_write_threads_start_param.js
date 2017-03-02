// This test ensures that the replWriterThreadCount server parameter:
//       1) cannot be less than 1
//       2) cannot be greater than 256
//       3) is actually set to the passed in value
//       4) cannot be altered at run time

(function() {
    "use strict";

    // too low a count
    clearRawBongoProgramOutput();
    var bongo = BongoRunner.runBongod({setParameter: 'replWriterThreadCount=0'});
    assert.soon(function() {
        return rawBongoProgramOutput().match("replWriterThreadCount must be between 1 and 256");
    }, "bongod started with too low a value for replWriterThreadCount");

    // too high a count
    clearRawBongoProgramOutput();
    bongo = BongoRunner.runBongod({setParameter: 'replWriterThreadCount=257'});
    assert.soon(function() {
        return rawBongoProgramOutput().match("replWriterThreadCount must be between 1 and 256");
    }, "bongod started with too high a value for replWriterThreadCount");

    // proper count
    clearRawBongoProgramOutput();
    bongo = BongoRunner.runBongod({setParameter: 'replWriterThreadCount=24'});
    assert.neq(null, bongo, "bongod failed to start with a suitable replWriterThreadCount value");
    assert(!rawBongoProgramOutput().match("replWriterThreadCount must be between 1 and 256"),
           "despite accepting the replWriterThreadCount value, bongod logged an error");

    // getParameter to confirm the value was set
    var result = bongo.getDB("admin").runCommand({getParameter: 1, replWriterThreadCount: 1});
    assert.eq(24, result.replWriterThreadCount, "replWriterThreadCount was not set internally");

    // setParameter to ensure it is not possible
    assert.commandFailed(
        bongo.getDB("admin").runCommand({setParameter: 1, replWriterThreadCount: 1}));
}());
