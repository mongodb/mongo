(function() {
    'use strict';

    load('jstests/replsets/libs/apply_ops_concurrent_non_atomic.js');

    new ApplyOpsConcurrentNonAtomicTest({
        ns1: 'test1.coll1',
        ns2: 'test2.coll2',
        requiresDocumentLevelConcurrency: false,
    }).run();
}());
