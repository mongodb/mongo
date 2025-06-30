// Verify that the serachCoordinator role has write priveleges only to __mdb_internal_search

function runTest(conn, role) {
    const user = role + 'User';
    const pwd = "pwd";

    const adminDb = conn.getDB('admin');
    const searchDb = conn.getDB('__mdb_internal_search');
    const someOtherDb = conn.getDB('someotherdb');
    const configDb = conn.getDB('config');

    assert.commandWorked(adminDb.runCommand({createUser: user, pwd: pwd, roles: [role]}));

    adminDb.logout();

    assert(adminDb.auth(user, pwd));

    let res = searchDb.test.insertOne({abc: 'def'});
    assert(res.acknowledged, "Expected searchCoordinator to insert into __mdb_internal_search");

    assert.throws(function() {
        someOtherDb.test.insertOne({abc: 'def'});
    }, [], "Expected searchCoordinator to fail insert into non-search DB collection");

    assert.throws(function() {
        configDb.search.insertOne({abc: 'def'});
    }, [], "Expected searchCoordinator to fail insert into config DB");

    jsTest.log(assert.commandWorked(adminDb.runCommand({connectionStatus: 1, showPrivileges: 1})));

    searchDb.logout();
}

const standalone = MongoRunner.runMongod({auth: ''});
runTest(standalone, "searchCoordinator");
MongoRunner.stopMongod(standalone);