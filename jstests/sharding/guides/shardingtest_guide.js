/**
 * This is meant to be a usage guide for the ShardingTest fixture. Check the shardingtest.js doc
 * comment for more detailed explanations on the possible options. You can run this test to see how
 * it works. Feel free to add to the guide.
 */
(function() {
'use strict';

/**
 * Default Configuration
 */
{
    // The default configuration is 2 shards deployed as single node replica sets, 1 mongos, 1
    // config server deployed as a 3-node replica set.
    let st = new ShardingTest({});
    st.stop();
}

/**
 * Basic Configurations
 */
{
    // We can define configurations by specifying specific values. The following configuration
    // starts 2 1-node replica set shards, 2 mongos, 1 2-node replica set config server.

    let st = new ShardingTest({shards: 2, mongos: 2, config: 2});
    st.stop();
}

{
    // We can define configurations by passing in an array of configuration objects.
    // Note: this restricts shards to be single server only.
    // The following configuration starts 2 1-node replica set shards, 2 mongos, 1 2-node
    // replica set config server. Each configuration object is specific to each node, other
    // parameters can be specified in the same fashion.

    let st = new ShardingTest({
        shards: [{verbose: 5}, {verbose: 3}],
        mongos: [{verbose: 5}, {verbose: 3}],
        config: [{verbose: 5}, {verbose: 3}]
    });
    st.stop();
}

{
    // We can specify configuration objects like below. This example starts 2 single node replica
    // set shards, 2 2-node replica set shards (4 shards total), 2 mongos, 1 3-node config server
    // with verbosity on all nodes turned up to 5.

    let st = new ShardingTest({
        shards: {
            d0: {verbose: 5 /* node parameters */},
            d1: {verbose: 5 /* node parameters */},
            rs2: {nodes: 2 /* ReplSetTest parameters */},
            rs3: {nodes: 2 /* ReplSetTest parameters */}
        },
        mongos: {s0: {verbose: 5 /* node parameters */}, s1: {verbose: 5 /* node parameters */}},
        config: {
            c0: {verbose: 5 /* node parameters */},
            c1: {verbose: 5 /* node parameters */},
            c2: {verbose: 5 /* node parameters */}
        }
    });

    // We can retrieve the connection strings of each component of the sharded cluster using the
    // ShardingTest object.
    print("shard0 connection string: " + tojson(st.shard0));
    print("shard1 connection string: " + tojson(st.shard1));
    print("shard2 connection string: " + tojson(st.shard2));
    print("shard3 connection string: " + tojson(st.shard3));
    print("mongos0 connection string: " + tojson(st.s0));
    print("mongos1 connection string: " + tojson(st.s1));
    print("configsvr replica node 0 connection string: " +
          tojson(st.c0));  // can also do st.config0
    print("configsvr replica node 1 connection string: " +
          tojson(st.c1));  // can also do st.config1
    print("configsvr replica node 2 connection string: " +
          tojson(st.c2));  // can also do st.config2

    // Some other connection strings we can retrieve from the ShardingTest object.
    print("first mongos connection string: " + tojson(st.s));
    print("rs0 connection string (referring to shard0's replica set): " + tojson(st.rs0.getURL()));
    print("rs1 connection string (referring to shard1's replica set): " + tojson(st.rs1.getURL()));
    print("rs2 connection string (referring to shard2's replica set): " + tojson(st.rs2.getURL()));
    print("rs3 connection string (referring to shard3's replica set): " + tojson(st.rs3.getURL()));
    print("configRS connection string: " + tojson(st.configRS.getURL()));

    // You can get the ReplSetTest object by doing (for a specific node, shard0 used as an
    // example below):

    let shard0RSParameter = st.rs0;

    // or by doing:

    let shard0ShardParameter = st.shard0.rs;
    assert.eq(shard0RSParameter, shard0ShardParameter);

    // We can get direct connections to the nodes of the replica set of some shard by using
    // either the 'rs' parameter or the 'shard' parameter.

    assert.eq(st.rs0.nodes[0], st.shard0.rs.nodes[0]);

    // Once we get the connection string we can use them to run commands. Below is an example
    // of an insert done on shard0.
    assert.commandWorked(
        st.shard0.getDB("test").runCommand({insert: 'testColl', documents: [{x: 1}]}));

    st.stop();
}

/**
 * Advanced configurations
 */

// We can specify options that are common to all nodes.
// Note: Only a few of the options are listed in section, refer to the shardingtest.js doc
// comment for all possible options.

{
    // We can specify a common replica set size for our shards. Below we start 2 3 node
    // replica set shards, 2 mongos, 1 single node replica set config server.

    let st = new ShardingTest(
        {shards: 2, mongos: 2, config: 1, rs: {nodes: 3, /* other ReplSetTest options */}});
    st.stop();
}

{
    // We can use the 'other' parameter to set special common options in the sharded cluster IE
    // enabling the balancer. Below we start a similar configuration to the one above except that
    // we enable the balancer and also set the minSnapshotHistoryWindowInSeconds parameter on all
    // the shards. The 'rsOptions' parameter behaves the same way as the 'rs' parameter.
    // Check the shardingtest.js doc comment to see other accepted parameters.

    const nodeOptions = {setParameter: {minSnapshotHistoryWindowInSeconds: 600}};

    let st = new ShardingTest({
        shards: 2,
        mongos: 2,
        config: 1,
        other: {
            rs: {nodes: 3, /* other ReplSetTest options */},
            enableBalancer: true,
            rsOptions: nodeOptions
        }
    });
    st.stop();
}
})();
