// Regression test for SERVER-7428.
//
// Verify that the copyDatabase command works appropriately when the
// target mongo instance has authentication enabled.

// Setup fromDb with no auth
var fromDb = MongoRunner.runMongod({ port: 29000 });
 
// Setup toDb with auth
var toDb = MongoRunner.runMongod({auth : "", port : 31001});
var admin = toDb.getDB("admin");
admin.addUser("foo","bar", jsTest.adminUserRoles);
admin.auth("foo","bar");
  
admin.copyDatabase('test', 'test', fromDb.host)

