doTest = function( signal ) {

    // Test startup with seed list
    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3, useSeedList: true} );

    var nodes = replTest.startSet();
    replTest.initiate();
    var master = replTest.getMaster();

    // End test
    replTest.stopSet( signal );
}

doTest( 15 );
