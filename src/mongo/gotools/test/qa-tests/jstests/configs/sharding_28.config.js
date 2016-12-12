load("jstests/configs/standard_dump_targets.config.js");

/* exported getToolTest */
var getToolTest;

(function() {
  getToolTest = function(name) {
    var toolTest = new ToolTest(name, null);

    var shardingTest = new ShardingTest({name: name,
        shards: 2,
        verbose: 0,
        mongos: 3,
        other: {
          chunksize: 1,
          enableBalancer: 0
        }
    });
    shardingTest.adminCommand({enablesharding: name});

    toolTest.m = shardingTest.s0;
    toolTest.db = shardingTest.getDB(name);
    toolTest.port = shardingTest.s0.port;

    var oldStop = toolTest.stop;
    toolTest.stop = function() {
      shardingTest.stop();
      oldStop.apply(toolTest, arguments);
    };

    toolTest.isSharded = true;

    return toolTest;
  };
}());

/* exported getCommonToolArguments */
var getCommonToolArguments = function() {
  return [];
};
