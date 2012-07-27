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
        
        this.upgradeNode( node, binVersion, true )
        
        if( noDowntimePossible )
            assert.eq( this.getNodeId( primary ), prevPrimaryId )
    }
}

ReplSetTest.prototype.upgradeNode = function( node, binVersion, waitForState ){
    
    var node = this.restart( node, { binVersion : binVersion } )
    
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
