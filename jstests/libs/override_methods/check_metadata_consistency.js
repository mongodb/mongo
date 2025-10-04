import {MetadataConsistencyChecker} from "jstests/libs/check_metadata_consistency_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

ShardingTest.prototype.checkMetadataConsistency = function () {
    if (jsTest.options().skipCheckMetadataConsistency) {
        jsTest.log("Skipped metadata consistency check: test disabled");
        return;
    }

    const mongos = new Mongo(this.s.host);
    MetadataConsistencyChecker.run(mongos);
};
