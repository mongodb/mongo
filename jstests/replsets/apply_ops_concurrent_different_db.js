/**
 * @tags: [
 * # 6.2 removes support for atomic applyOps
 * requires_fcv_62,
 * ]
 */
(function() {
'use strict';

load('jstests/replsets/libs/apply_ops_concurrent.js');

new ApplyOpsConcurrentTest({ns1: 'test1.coll1', ns2: 'test2.coll2'}).run();
}());
