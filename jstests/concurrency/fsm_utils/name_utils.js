/**
 * Helpers for generating names of databases and collections
 * to execute workloads against.
 * The DB and collections names here are synchronized with the
 * the names found in the CleanupConcurrencyWorkloads hook
 * in resmoke.
 */

// Returns a unique database name:
//   <dbNamePrefix>fsmdb0, <dbNamePrefix>fsmdb1, ...
export var uniqueDBName = (function(dbNamePrefix) {
    var i = 0;

    return function(dbNamePrefix) {
        var prefix = dbNamePrefix || '';
        return prefix + 'fsmdb' + i++;
    };
})();

// Returns a unique collection name:
//   fsmcoll0, fsmcoll1, ...
export var uniqueCollName = (function() {
    var i = 0;

    return function() {
        return 'fsmcoll' + i++;
    };
})();
