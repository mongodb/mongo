// Regression test for SERVER-7428.

// TODO(spencer): move this test out of slowNightly directory once there is a better place for tests
// that start their own bongod's but aren't slow

// Verify that the copyDatabase command works appropriately when the
// target bongo instance has authentication enabled.

(function() {

    // Setup fromDb with no auth
    var fromDb = BongoRunner.runBongod();

    // Setup toDb with auth
    var toDb = BongoRunner.runBongod({auth: ""});
    var admin = toDb.getDB("admin");
    admin.createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles});
    admin.auth("foo", "bar");

    admin.copyDatabase('test', 'test', fromDb.host);

})();
