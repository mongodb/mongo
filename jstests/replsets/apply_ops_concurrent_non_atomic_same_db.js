(function() {
    'use strict';

    load('jstests/replsets/libs/apply_ops_concurrent_non_atomic.js');

    new ApplyOpsConcurrentNonAtomicTest({
        ns1: 'test.coll1',
        ns2: 'test.coll2',
        requiresDocumentLevelConcurrency: false,
    }).run();
}());
