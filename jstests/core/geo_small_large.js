// SERVER-2386, general geo-indexing using very large and very small bounds

load("jstests/libs/geo_near_random.js");

// Do some random tests (for near queries) with very large and small ranges

var test = new GeoNearRandomTest("geo_small_large");

bounds = {
    min: -Math.pow(2, 34),
    max: Math.pow(2, 34)
};

test.insertPts(50, bounds);

printjson(db["geo_small_large"].find().limit(10).toArray());

test.testPt([0, 0]);
test.testPt(test.mkPt(undefined, bounds));
test.testPt(test.mkPt(undefined, bounds));
test.testPt(test.mkPt(undefined, bounds));
test.testPt(test.mkPt(undefined, bounds));

test = new GeoNearRandomTest("geo_small_large");

bounds = {
    min: -Math.pow(2, -34),
    max: Math.pow(2, -34)
};

test.insertPts(50, bounds);

printjson(db["geo_small_large"].find().limit(10).toArray());

test.testPt([0, 0]);
test.testPt(test.mkPt(undefined, bounds));
test.testPt(test.mkPt(undefined, bounds));
test.testPt(test.mkPt(undefined, bounds));
test.testPt(test.mkPt(undefined, bounds));

// Check that our box and circle queries also work
var scales = [Math.pow(2, 40), Math.pow(2, -40), Math.pow(2, 2), Math.pow(3, -15), Math.pow(3, 15)];

for (var i = 0; i < scales.length; i++) {
    var scale = scales[i];

    var eps = Math.pow(2, -7) * scale;
    var radius = 5 * scale;
    var max = 10 * scale;
    var min = -max;
    var range = max - min;
    var bits = 2 + Math.random() * 30;

    var t = db["geo_small_large"];
    t.drop();
    t.ensureIndex({p: "2d"}, {min: min, max: max, bits: bits});

    var outPoints = 0;
    var inPoints = 0;

    printjson({eps: eps, radius: radius, max: max, min: min, range: range, bits: bits});

    // Put a point slightly inside and outside our range
    for (var j = 0; j < 2; j++) {
        var currRad = (j % 2 == 0 ? radius + eps : radius - eps);
        var res = t.insert({p: {x: currRad, y: 0}});
        print(res.toString());
    }

    printjson(t.find().toArray());

    assert.eq(
        t.count({p: {$within: {$center: [[0, 0], radius]}}}), 1, "Incorrect center points found!");
    assert.eq(t.count({p: {$within: {$box: [[-radius, -radius], [radius, radius]]}}}),
              1,
              "Incorrect box points found!");

    var shouldFind = [];
    var randoms = [];

    for (var j = 0; j < 2; j++) {
        var randX = Math.random();  // randoms[j].randX
        var randY = Math.random();  // randoms[j].randY

        randoms.push({randX: randX, randY: randY});

        var x = randX * (range - eps) + eps + min;
        var y = randY * (range - eps) + eps + min;

        t.insert({p: [x, y]});

        if (x * x + y * y > radius * radius) {
            // print( "out point ");
            // printjson({ x : x, y : y })
            outPoints++;
        } else {
            // print( "in point ");
            // printjson({ x : x, y : y })
            inPoints++;
            shouldFind.push({x: x, y: y, radius: Math.sqrt(x * x + y * y)});
        }
    }

    /*
    function printDiff( didFind, shouldFind ){

        for( var i = 0; i < shouldFind.length; i++ ){
            var beenFound = false;
            for( var j = 0; j < didFind.length && !beenFound ; j++ ){
                beenFound = shouldFind[i].x == didFind[j].x &&
                            shouldFind[i].y == didFind[j].y
            }

            if( !beenFound ){
                print( "Could not find: " )
                shouldFind[i].inRadius = ( radius - shouldFind[i].radius >= 0 )
                printjson( shouldFind[i] )
            }
        }
    }

    print( "Finding random pts... ")
    var found = t.find( { p : { $within : { $center : [[0, 0], radius ] } } } ).toArray()
    var didFind = []
    for( var f = 0; f < found.length; f++ ){
        //printjson( found[f] )
        var x = found[f].p.x != undefined ? found[f].p.x : found[f].p[0]
        var y = found[f].p.y != undefined ? found[f].p.y : found[f].p[1]
        didFind.push({ x : x, y : y, radius : Math.sqrt( x * x + y * y ) })
    }

    print( "Did not find but should: ")
    printDiff( didFind, shouldFind )
    print( "Found but should not have: ")
    printDiff( shouldFind, didFind )
    */

    assert.eq(t.count({p: {$within: {$center: [[0, 0], radius]}}}),
              1 + inPoints,
              "Incorrect random center points found!\n" + tojson(randoms));

    print("Found " + inPoints + " points in and " + outPoints + " points out.");

    var found = t.find({p: {$near: [0, 0], $maxDistance: radius}}).toArray();
    var dist = 0;
    for (var f = 0; f < found.length; f++) {
        var x = found[f].p.x != undefined ? found[f].p.x : found[f].p[0];
        var y = found[f].p.y != undefined ? found[f].p.y : found[f].p[1];
        print("Dist: x : " + x + " y : " + y + " dist : " + Math.sqrt(x * x + y * y) +
              " radius : " + radius);
    }

    assert.eq(t.count({p: {$near: [0, 0], $maxDistance: radius}}),
              1 + inPoints,
              "Incorrect random center points found near!\n" + tojson(randoms));
}
