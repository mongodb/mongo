//
// Test that privileges are dropped when a user is dropped and re-added while another
// client is logged in under that username (against a sharded cluster).
//

load('./jstests/libs/drop_user_while_logged_in.js');

var st = new ShardingTest({shards: 2, mongos: 1, keyFile: 'jstests/libs/key1'});
var mongos = st.s0;

testDroppingUsersWhileLoggedIn(mongos);
