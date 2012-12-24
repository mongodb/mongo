var conn = MongoRunner.runMongod({ noscripting: '' });
var testDB = conn.getDB( 'foo' );
var coll = testDB.bar;

coll.insert({ x: 1 });

var map = function() {
    emit( this.x, 1 );
};

var reduce = function( key, values ) {
    return 1;
};

var mrResult = testDB.runCommand({ mapReduce: 'bar', map: map, reduce: reduce,
                                   out: { inline: 1 }});

assert.eq( 0, mrResult.ok, 'mr result: ' + tojson( mrResult ));

// Confirm that mongod did not crash
var cmdResult = testDB.adminCommand({ serverStatus: 1 });
assert( cmdResult.ok, 'serverStatus failed, result: ' +
        tojson( cmdResult ));

