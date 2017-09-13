(function() {
    'use strict';

    load('jstests/replsets/libs/apply_ops_concurrent_non_atomic.js');

    new ApplyOpsConcurrentNonAtomicTest({
        ns1: 'test.coll',
        ns2: 'test.coll',
        requiresDocumentLevelConcurrency: true,
    }).run();
}());
