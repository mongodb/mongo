

(function() {
'use strict';

var st = new ShardingTest({
    mongos: 1,
    shards: 1,
});

const result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
print(tojson(result));

assert(result.state == "StartupCheck" || result.state == "Ok");
})();
