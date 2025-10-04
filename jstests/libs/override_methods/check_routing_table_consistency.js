import {RoutingTableConsistencyChecker} from "jstests/libs/check_routing_table_consistency_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

ShardingTest.prototype.checkRoutingTableConsistency = function () {
    if (jsTest.options().skipCheckRoutingTableConsistency) {
        jsTest.log("Skipping routing table consistency check");
        return;
    }

    const mongos = new Mongo(this.s.host);
    mongos.fullOptions = this.s.fullOptions || {};
    mongos.setReadPref("primaryPreferred");

    RoutingTableConsistencyChecker.run(mongos);
};
