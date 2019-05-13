// Test config file expansion using EXEC with digests.

(function() {
    'use strict';

    load('jstests/noPassthrough/libs/configExpand/lib.js');

    // hash === SHA256HMAC('12345', 'secret')
    const hash = 'f88c7ebe4740db59c873cecf5e1f18e3726a1ad64068a13d764b79028430ab0e';

    // Simple positive case.
    configExpandSuccess({
        setParameter: {
            scramIterationCount:
                {__exec: makeReflectionCmd('12345'), digest: hash, digest_key: '736563726574'}
        }
    });

    // Invalid digest length.
    configExpandFailure({
        setParameter: {
            scramIteratorCount:
                {__exec: makeReflectionCmd('12345'), digest: '123', digest_key: '736563726574'}
        }
    },
                        /digest: Not a valid, even length hex string/);

    // Invalid characters.
    configExpandFailure({
        setParameter: {
            scramIteratorCount:
                {__exec: makeReflectionCmd('12345'), digest: hash, digest_key: '736563X26574'}
        }
    },
                        /digest_key: Not a valid, even length hex string/);

    // Digest without key.
    configExpandFailure(
        {setParameter: {scramIteratorCount: {__exec: makeReflectionCmd('12345'), digest: hash}}},
        /digest requires digest_key/);

    // Empty digest_key.
    configExpandFailure({
        setParameter: {
            scramIteratorCount:
                {__exec: makeReflectionCmd('12345'), digest: hash, digest_key: ''}
        }
    },
                        /digest_key must not be empty/);

    // Mismatched digests.
    configExpandFailure({
        setParameter: {
            scramIteratorCount:
                {__exec: makeReflectionCmd('12345'), digest: hash, digest_key: '736563726575'}
        }
    },
                        /does not match expected digest/);

})();
