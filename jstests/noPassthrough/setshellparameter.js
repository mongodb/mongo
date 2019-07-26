// Test --setShellParameter CLI switch.

(function() {
'use strict';

function test(ssp, succeed) {
    const result = runMongoProgram('./mongo', '--setShellParameter', ssp, '--nodb', '--eval', ';');
    assert.eq(
        0 == result, succeed, '--setShellParameter ' + ssp + 'worked/didn\'t-work unexpectedly');
}

// Whitelisted
test('disabledSecureAllocatorDomains=foo', true);

// Not whitelisted
test('enableTestCommands=1', false);

// Unknown
test('theAnswerToTheQuestionOfLifeTheUniverseAndEverything=42', false);
})();
