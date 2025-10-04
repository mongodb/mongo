// @tags: [
//   assumes_balancer_off,
//   requires_fastcount,
//   requires_getmore,
// ]

let t = db.geo_center_sphere1;

function test(index) {
    t.drop();
    let skip = 8; // lower for more rigor, higher for more speed (tested with .5, .678, 1, 2, 3, and 4)

    let searches = [
        //  x , y    rad
        [[5, 0], 0.05], // ~200 miles
        [[135, 0], 0.05],

        [[5, 70], 0.05],
        [[135, 70], 0.05],
        [[5, 85], 0.05],

        [[20, 0], 0.25], // ~1000 miles
        [[20, -45], 0.25],
        [[-20, 60], 0.25],
        [[-20, -70], 0.25],
    ];
    let correct = searches.map(function (z) {
        return [];
    });

    let num = 0;

    let bulk = t.initializeUnorderedBulkOp();
    for (let x = -179; x <= 179; x += skip) {
        for (let y = -89; y <= 89; y += skip) {
            let o = {_id: num++, loc: [x, y]};
            bulk.insert(o);
            for (let i = 0; i < searches.length; i++) {
                if (Geo.sphereDistance([x, y], searches[i][0]) <= searches[i][1]) correct[i].push(o);
            }
        }
        gc(); // needed with low skip values
    }
    assert.commandWorked(bulk.execute());

    if (index) {
        t.createIndex({loc: index});
    }

    for (let i = 0; i < searches.length; i++) {
        print("------------");
        print(tojson(searches[i]) + "\t" + correct[i].length);
        let q = {loc: {$within: {$centerSphere: searches[i]}}};

        // correct[i].forEach( printjson )
        // printjson( q );
        // t.find( q ).forEach( printjson )

        // printjson(t.find( q ).explain())

        // printjson( correct[i].map( function(z){ return z._id; } ).sort() )
        // printjson( t.find(q).map( function(z){ return z._id; } ).sort() )

        let numExpected = correct[i].length;
        let x = correct[i].map(function (z) {
            return z._id;
        });
        let y = t.find(q).map(function (z) {
            return z._id;
        });

        let missing = [];
        let epsilon = 0.001; // allow tenth of a percent error due to conversions
        for (var j = 0; j < x.length; j++) {
            if (!Array.contains(y, x[j])) {
                missing.push(x[j]);
                var obj = t.findOne({_id: x[j]});
                var dist = Geo.sphereDistance(searches[i][0], obj.loc);
                print("missing: " + tojson(obj) + " " + dist);
                if (Math.abs(dist - searches[i][1]) / dist < epsilon) numExpected -= 1;
            }
        }
        for (var j = 0; j < y.length; j++) {
            if (!Array.contains(x, y[j])) {
                missing.push(y[j]);
                var obj = t.findOne({_id: y[j]});
                var dist = Geo.sphereDistance(searches[i][0], obj.loc);
                print("extra: " + tojson(obj) + " " + dist);
                if (Math.abs(dist - searches[i][1]) / dist < epsilon) numExpected += 1;
            }
        }

        assert.eq(numExpected, t.find(q).itcount(), "itcount : " + tojson(searches[i]));
        assert.eq(numExpected, t.find(q).count(), "count : " + tojson(searches[i]));
        if (index == "2d") {
            let explain = t.find(q).explain("executionStats");
            print("explain for " + tojson(q, "", true) + " = " + tojson(explain));
            // The index should be at least minimally effective in preventing the full collection
            // scan.
            assert.gt(t.find().count(), explain.executionStats.totalKeysExamined, "nscanned : " + tojson(searches[i]));
        }
    }
}

test("2d");
test("2dsphere");
test(false);
