(function () {
    "use strict";

    var mongosOpts, configOpts, shardOpts;

    var oldVersion = "2.6";
    var newVersion = "latest";

    mongosOpts = { binVersion: newVersion };
    configOpts = { binVersion: newVersion };
    shardOpts = [
        { binVersion: oldVersion }
    ];

    var options = {
        separateConfig : true,
        sync : false
    };
    var st = new ShardingTest({
        shards: shardOpts,
        mongos: mongosOpts,
        config : configOpts,
        other: options
    });

    st.s.adminCommand( { enableSharding : "test" } );
    st.printShardingStatus();

    db = st.s.getDB("test");

    load("jstests/core/list_collections1.js");

    load("jstests/libs/list_indexes_lib.js");
    basicTest();
    invalidValueTest();
    nonexistentDatabaseTest();
    cornerCaseTest();

}());
