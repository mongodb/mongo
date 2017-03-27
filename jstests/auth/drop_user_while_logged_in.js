//
// Test that privileges are dropped when a user is dropped and re-added while another
// client is logged in under that username (against a standalone).
//

load('./jstests/libs/drop_user_while_logged_in.js');

var conn = MongoRunner.runMongod({auth: "", smallfiles: ""});
testDroppingUsersWhileLoggedIn(conn);
