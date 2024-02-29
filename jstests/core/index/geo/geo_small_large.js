/**
 * This test ensures that geo indexes can be defined using very large and very small bounds.
 * Additionally, this test also randomizes the precision for the geo index specified in
 * 'bits' index option. See SERVER-2386.
 *
 * @tags: [
 *  # GeoNearRandomTest::testPt() uses find().toArray() that makes use of a cursor
 *  requires_getmore,
 * ]
 */
import {GeoNearRandomTest} from "jstests/libs/geo_near_random.js";

// Do some random tests (for near queries) with very large and small ranges

let test = new GeoNearRandomTest("geo_small_large");

let bounds = {min: -Math.pow(2, 34), max: Math.pow(2, 34)};

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
let scales = [Math.pow(2, 40), Math.pow(2, -40), Math.pow(2, 2), Math.pow(3, -15), Math.pow(3, 15)];

for (let i = 0; i < scales.length; i++) {
    const scale = scales[i];

    const eps = Math.pow(2, -7) * scale;
    const radius = 5 * scale;
    const max = 10 * scale;
    const min = -max;
    const range = max - min;
    const bits = 2 + Math.random() * 30;

    const t = db.getCollection('geo_small_large_' + i);
    t.drop();
    assert.commandWorked(t.createIndex({p: "2d"}, {min: min, max: max, bits: bits}));

    let outPoints = 0;
    let inPoints = 0;

    printjson({eps: eps, radius: radius, max: max, min: min, range: range, bits: bits});

    // Put a point slightly inside and outside our range
    for (let j = 0; j < 2; j++) {
        const currRad = (j % 2 == 0 ? radius + eps : radius - eps);
        const res = t.insert({p: {x: currRad, y: 0}});
        print(res.toString());
    }

    printjson(t.find().toArray());

    assert.eq(
        t.count({p: {$within: {$center: [[0, 0], radius]}}}), 1, "Incorrect center points found!");
    assert.eq(t.count({p: {$within: {$box: [[-radius, -radius], [radius, radius]]}}}),
              1,
              "Incorrect box points found!");

    let shouldFind = [];
    let randoms = [];

    for (let j = 0; j < 2; j++) {
        const randX = Math.random();  // randoms[j].randX
        const randY = Math.random();  // randoms[j].randY

        randoms.push({randX: randX, randY: randY});

        const x = randX * (range - eps) + eps + min;
        const y = randY * (range - eps) + eps + min;

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

        for( let i = 0; i < shouldFind.length; i++ ){
            let beenFound = false;
            for( let j = 0; j < didFind.length && !beenFound ; j++ ){
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
    let found = t.find( { p : { $within : { $center : [[0, 0], radius ] } } } ).toArray()
    let didFind = []
    for( let f = 0; f < found.length; f++ ){
        //printjson( found[f] )
        let x = found[f].p.x != undefined ? found[f].p.x : found[f].p[0]
        let y = found[f].p.y != undefined ? found[f].p.y : found[f].p[1]
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

    const found = t.find({p: {$near: [0, 0], $maxDistance: radius}}).toArray();
    let dist = 0;
    for (let f = 0; f < found.length; f++) {
        let x = found[f].p.x != undefined ? found[f].p.x : found[f].p[0];
        let y = found[f].p.y != undefined ? found[f].p.y : found[f].p[1];
        print("Dist: x : " + x + " y : " + y + " dist : " + Math.sqrt(x * x + y * y) +
              " radius : " + radius);
    }

    assert.eq(t.count({p: {$near: [0, 0], $maxDistance: radius}}),
              1 + inPoints,
              "Incorrect random center points found near!\n" + tojson(randoms));
}