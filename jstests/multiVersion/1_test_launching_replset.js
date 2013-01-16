//
// Tests launching multi-version ReplSetTest replica sets
//

// Check our latest versions
var versionsToCheck = [ "last-stable",
                        "latest" ];

load('./jstests/multiVersion/libs/verify_versions.js');

jsTest.log( "Testing legacy versions..." )

for( var i = 0; i < versionsToCheck.length; i++ ){

    var version = versionsToCheck[ i ]
    
    // Set up a replica set
    
    var rst = new ReplSetTest({ nodes : 2 })
    
    rst.startSet({ binVersion : version })
        
    var nodes = rst.nodes
    
    // Make sure the started versions are actually the correct versions
    for( var j = 0; j < nodes.length; j++ ) assert.binVersion(nodes[j], version);
    
    rst.stopSet()
}

jsTest.log( "Testing mixed versions..." )

// Set up a multi-version replica set

var rst = new ReplSetTest({ nodes : 2 })

rst.startSet({ binVersion : versionsToCheck })

var nodes = rst.nodes

//Make sure we have hosts of all the different versions
var versionsFound = []
for( var j = 0; j < nodes.length; j++ )
    versionsFound.push(nodes[j].getBinVersion());

assert.allBinVersions(versionsToCheck, versionsFound);

rst.stopSet()

jsTest.log( "Done!" )

//
// End
//
