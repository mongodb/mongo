// SERVER-12886 Ensure that small updates to large documents don't re-write the document

// enable all profiling
db.setProfilingLevel( 2 );

// helper function to return the last operation from the profiling collection
function getLastOp() {
    var cursor = db.system.profile.find( { ns : db.docs.getFullName() , op : "update" } );
    cursor = cursor.sort( { $natural : -1 } ).limit(1);
    return cursor[0];
}

function getMBWritten() {
    return db.adminCommand('serverStatus').dur.writeToDataFilesMB;
}

function waitForIO(target) {
    mbWritten = getMBWritten()
    while (mbWritten <= target) {
        sleep(500);
        mbWritten = getMBWritten()
    }
    assert(mbWritten >0, "error: data write should be present", mbWritten)
    return mbWritten
}

function waitForQuiet() {
    mbWritten = getMBWritten()
    while (mbWritten > 0) {
        sleep(500);
        mbWritten = getMBWritten()
    }
    assert(mbWritten < 1, "error: stats should be quiet", mbWritten)
    return mbWritten
}

function doUpdate(with_index, u) {
    waitForQuiet();

    // perform the actual update
    db.docs.update( { '_id' : 1 }, u);

    // print update and if it was fastmod or not
    printjson( u );
    var op = getLastOp();
    w = op['lockStats']['timeLockedMicros']['w'];
    mbWritten = waitForIO(0);
    if (op.nModified == 0) {
        operation = "no-op"; 
    } else if (op.fastmod) {
        operation = "fastmod"; 
    } else {
        operation = "slowmod"; 
    }
    print('index:', with_index, 'op:', operation, 'lock:', w, 'MB:', mbWritten)
}


var initial_doc = { '_id' : 1, 'arr' : [ 1 ], 'empty_arr': [ ], 'int' : 1, payload: new Array(1024*1024*10).join('x') };


function testUpdateSmallFieldInLargeDocument(intial_doc, u, with_index) {
    // drop db, insert initial document
    db.docs.drop();
    db.docs.insert( initial_doc );

    // Wait to see the initial write
    mbWritten = waitForIO(5);
    assert(mbWritten > 5, "should pick up the initial write", mbWritten)
    // Wait for quiet in the database
    waitForQuiet();

    if (with_index) {
        // create indexes  (comment out for non-index case)
        db.docs.ensureIndex({'arr': 1})
        db.docs.ensureIndex({'empty_arr': 1})
        db.docs.ensureIndex({'int': 1})
    }

    // Perform an initial update
    doUpdate(with_index, u);

    // Run the operation again. In this case, both should produce little I/O
    doUpdate(with_index, u);

    // Assert that we haven't produced much I/O
    assert(mbWritten < 1, "Small Update produced > 1MB write!")

    print();
}


var updates = [ 

    { '$inc' : { 'int' : 1 } },           // increase int
    { '$rename' : { 'int' : 'abc' } },    // rename a field (same length)
    { '$rename' : { 'int' : 'abcd' } },    // rename a field (diff length)
    { '$set' : { 'foo' : 1 } },           // set non-existing field
    { '$set' : { 'int' : 5 } },           // set existing field, different value
    { '$set' : { 'arr' : [ 2 ] } },       // set entire array of same size/type but different values

    // array operators

    { '$addToSet' : { 'arr' : 2 } },      // add new value to set
    { '$pop' : { 'arr' : 1 } },           // pop from existing non-empty array
    { '$pull' : { 'arr' : 1 } },          // pull matching value
    { '$push' : { 'arr' : 1 } },          // push new value to array

    // field operators on array fields

    { '$inc' : { 'arr.0' : 1 } },         // increase int in array
    { '$set' : { 'arr.1' : 1 } },         // set non-existing field in array
    { '$set' : { 'arr.0' : 1 } },         // set existing field in array, same value (no-op)
    { '$set' : { 'arr.0' : 2 } },         // set existing field in array, different value
    { '$unset' : { 'arr.0' : 1 } },       // unset existing field in array
    { '$unset' : { 'arr.1' : 1 } }        // unset non-existing field in array (no-op)
]

var with_index = [true, false];

with_index.forEach(function(t) {
    updates.forEach(function (u) {
        testUpdateSmallFieldInLargeDocument(initial_doc, u, t);
    })
})
