'use strict';

/**
 * Helpers for generating names of databases and collections
 * to execute workloads against.
 */

if (typeof uniqueDBName === 'undefined') {
    // Returns a unique database name:
    //   <dbNamePrefix>db0, <dbNamePrefix>db1, ...
    var uniqueDBName = (function(dbNamePrefix) {
        var i = 0;

        return function(dbNamePrefix) {
            var prefix = dbNamePrefix || '';
            return prefix + 'db' + i++;
        };
    })();
}

if (typeof uniqueCollName === 'undefined') {
    // Returns a unique collection name:
    //   coll0, coll1, ...
    var uniqueCollName = (function() {
        var i = 0;

        return function() {
            return 'coll' + i++;
        };
    })();
}
