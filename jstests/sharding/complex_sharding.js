// Test of complex sharding initialization

var st = new ShardingTest( {
    
    mongos : { s0 : { verbose : 1 }, s1 : { verbose : 2 } },
    config : { c0 : { verbose : 3 } },
    shards : { d0 : { verbose : 4 }, 
               rs1 : {
                   nodes : { d0 : { verbose : 5 }, 
                             a1 : { verbose : 6 } } }
             }
    
} )

var s0 = st.s0
assert.eq( s0, st._mongos[0] )

var s1 = st.s1
assert.eq( s1, st._mongos[1] )

var c0 = st.c0
assert.eq( c0, st._configServers[0] )

var d0 = st.d0
assert.eq( d0, st._shardServers[0] )

var rs1 = st.rs1
assert.eq( rs1, st._rsObjects[1] )

var rs1_d0 = rs1.nodes[0]
var rs1_a1 = rs1.nodes[1]

assert.contains( "-v", s0.commandLine )
assert.contains( "-vv", s1.commandLine )
assert.contains( "-vvv", c0.commandLine )
assert.contains( "-vvvv", d0.commandLine )
assert.contains( "-vvvvv", rs1_d0.commandLine )
assert.contains( "-vvvvvv", rs1_a1.commandLine )

st.stop()



