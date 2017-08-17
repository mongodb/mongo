/**
 * Validate various native types continue to work when run in JITed code.
 *
 * In SERVER-30362, the JIT would not compile natives types which had custom getProperty
 * implementations correctly. We force the JIT to kick in by using large loops.
 */
(function() {
    'use strict';

    function testDBCollection() {
        const c = new DBCollection(null, null, "foo", "test.foo");
        for (let i = 0; i < 100000; i++) {
            if (c.toString() != "test.foo") {
                throw i;
            }
        }
    }

    function testDB() {
        const c = new DB(null, "test");
        for (let i = 0; i < 100000; i++) {
            if (c.toString() != "test") {
                throw i;
            }
        }
    }

    function testDBQuery() {
        const c = DBQuery('a', 'b', 'c', 'd');
        for (let i = 0; i < 100000; i++) {
            if (c.toString() != "DBQuery: d -> null") {
                throw i;
            }
        }
    }

    testDBCollection();
    testDB();
    testDBQuery();
})();