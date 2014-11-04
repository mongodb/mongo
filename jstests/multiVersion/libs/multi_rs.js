//
// Utility functions for multi-version replica sets
// 

ReplSetTest.prototype.upgradeSet = function( binVersion, options ){
    
    options = options || {}
    if( options.primaryStepdown == undefined ) options.primaryStepdown = true
    
    var nodes = this.nodes
    var primary = this.getPrimary()
    
    // Upgrade secondaries first
    var nodesToUpgrade = this.getSecondaries()
    
    // Then upgrade primaries
    nodesToUpgrade.push( primary )
    
    // We can upgrade with no primary downtime if we have enough nodes
    var noDowntimePossible = nodes.length > 2
    
    for( var i = 0; i < nodesToUpgrade.length; i++ ){
        
        var node = nodesToUpgrade[ i ]
        
        if( node == primary && options.primaryStepdown ){
            
            node = this.stepdown( node )
            primary = this.getPrimary()
        }
        
        var prevPrimaryId = this.getNodeId( primary )
        
        if( options.custom ){
            options.custom.binVersion = binVersion;

            for( var nodeName in this.nodeOptions ){
                this.nodeOptions[ nodeName ] = options.custom
            }
        }

        this.upgradeNode( node, binVersion, true, options )
        
        if( noDowntimePossible )
            assert.eq( this.getNodeId( primary ), prevPrimaryId )
    }
}

ReplSetTest.prototype.upgradeNode = function( node, binVersion, waitForState, options ){

    var node;
    if (options.custom) {
        node = this.restart( node, options.custom );
    } else {
        node = this.restart( node, { binVersion : binVersion } );
    }

    if (options.auth) {
        // Hardcode admin database, because otherwise can't get repl set status
        node.getDB("admin").auth(options.auth);
    }
    
    // By default, wait for primary or secondary state
    if( waitForState == undefined ) waitForState = true
    if( waitForState == true ) waitForState = [ ReplSetTest.State.PRIMARY, 
                                                ReplSetTest.State.SECONDARY,
                                                ReplSetTest.State.ARBITER ]
    if( waitForState )
        this.waitForState( node, waitForState )
    
    return node
}

ReplSetTest.prototype.stepdown = function( nodeId ){
        
    nodeId = this.getNodeId( nodeId )
    
    assert.eq( this.getNodeId( this.getPrimary() ), nodeId )    
    
    var node = this.nodes[ nodeId ]
    
    try {
        node.getDB("admin").runCommand({ replSetStepDown: 50, force : true })
        assert( false )
    }
    catch( e ){
        printjson( e );
    }
    
    return this.reconnect( node )
}

ReplSetTest.prototype.reconnect = function( node ){
    
    var nodeId = this.getNodeId( node )
    
    this.nodes[ nodeId ] = new Mongo( node.host )
    
    // TODO
    var except = {}
    
    for( var i in node ){
        if( typeof( node[i] ) == "function" ) continue
        this.nodes[ nodeId ][ i ] = node[ i ]
    }
    
    return this.nodes[ nodeId ]
}

ReplSetTest.prototype.conf = function () {
    var admin = this.getPrimary().getDB('admin');

    var resp = admin.runCommand({replSetGetConfig:1});

    if (resp.ok && !(resp.errmsg) && resp.config)
        return resp.config;

    else if (resp.errmsg && resp.errmsg.startsWith( "no such cmd" ))
        return admin.getSisterDB("local").system.replset.findOne();

    throw new Error("Could not retrieve replica set config: " + tojson(resp));
}
