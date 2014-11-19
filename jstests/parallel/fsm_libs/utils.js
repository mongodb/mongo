// Returns a unique database name:
//   db0, db1, ...
var uniqueDBName = (function() {
    var i = 0;

    return function() {
        return 'db' + i++;
    };
})();

// Returns a unique collection name:
//   coll0, coll1, ...
var uniqueCollName = (function() {
    var i = 0;

    return function() {
        return 'coll' + i++;
    };
})();
