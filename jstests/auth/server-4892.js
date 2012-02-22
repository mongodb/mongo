/*
 * Regression test for SERVER-4892.
 *
 * Verify that a client can delete cursors that it creates, when mongod is running with "auth"
 * enabled.
 */

var baseName = 'jstests_auth_server4892';
var dbpath = '/data/db/' + baseName;
var port = allocatePorts( 1 )[ 0 ];
var mongod_common_args = [
    '--port', port, '--dbpath', dbpath, '--bind_ip', '127.0.0.1', '--nohttpinterface' ];

/*
 * Start an instance of mongod, pass it as a parameter to operation(), then stop the instance of
 * mongod before unwinding or returning out of with_mongod().
 *
 * extra_mongod_args are extra arguments to pass on the mongod command line, in an Array.
 */
function with_mongod( extra_mongod_args, operation ) {
    var mongod = startMongoProgram.apply(
        null, ['mongod'].concat( mongod_common_args, extra_mongod_args ) );

    try {
        operation( mongod );
    } finally {
        stopMongod( port );
    }
}

/*
 * Fail an assertion if the given "mongod" instance does not have exactly expectNumLiveCursors live
 * cursors on the server.
 */
function expectNumLiveCursors(mongod, expectedNumLiveCursors) {
    var conn = new Mongo( mongod.host );
    var db = mongod.getDB( 'admin' );
    db.auth( 'admin', 'admin' );
    var actualNumLiveCursors = db.serverStatus().cursors.totalOpen;
    assert( actualNumLiveCursors == expectedNumLiveCursors,
          "actual num live cursors (" + actualNumLiveCursors + ") != exptected ("
          + expectedNumLiveCursors + ")");
}

resetDbpath( dbpath );

with_mongod( ['--noauth'], function setupTest( mongod ) {
    var admin, somedb, conn;
    conn = new Mongo( mongod.host );
    admin = conn.getDB( 'admin' );
    somedb = conn.getDB( 'somedb' );
    admin.addUser( 'admin', 'admin' );
    somedb.addUser( 'frim', 'fram' );
    somedb.data.drop();
    for (var i = 0; i < 10; ++i) {
        somedb.data.insert( { val: i } );
        assert ( ! somedb.getLastError() );
    }
} );

with_mongod( ['--auth'], function runTest( mongod ) {
    var conn = new Mongo( mongod.host );
    var somedb = conn.getDB( 'somedb' );
    somedb.auth('frim', 'fram');

    expectNumLiveCursors( mongod, 0 );

    var cursor = somedb.data.find({}, ['_id']).batchSize(1);
    cursor.next();
    expectNumLiveCursors( mongod, 1 );

    cursor = null;
    // NOTE(schwerin): We assume that after setting cursor = null, there are no remaining references
    // to the cursor, and that gc() will deterministically garbage collect it.
    gc();

  // NOTE(schwerin): dbKillCursors gets piggybacked on subsequent messages on the connection, so we
  // have to force a message to the server.
    somedb.data.findOne();

    expectNumLiveCursors( mongod, 0 );
});

