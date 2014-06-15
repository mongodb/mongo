// Ensures addition of node when passing object but no _id 

doTest = function( signal ) {

    var replTest = new ReplSetTest( {name: 'unicomplex', nodes: 3} );
    var nodes = replTest.nodeList();

    print(tojson(nodes));

    var conns = replTest.startSet();
    var r = replTest.initiate({"_id" : "unicomplex",
                "members" : [
                    {"_id" : 0, "host" : nodes[0] },
                    {"_id" : 1, "host" : nodes[1], "arbiterOnly" : true, "votes": 1, "priority" : 0}]});

    // Make sure we have a master
    var master = replTest.getMaster();
    
    //Add new node via document style
	var r2 = rs.add({ "host" : nodes[2] })
	assert(result.ok,1)
	
	var rs_data = {id:"",host:""};
	rs.status.members.forEach(function(node){ 
		if (node.host == nodes[2]){
			rs_data = { _id : node._id, host:node.host };
		}
	});
	
	//Lets make sure we got the _id we expected
	assert.eq(rs_data._id, 2);
    
    
    replTest.stopSet( signal );
}

doTest( 15 );
