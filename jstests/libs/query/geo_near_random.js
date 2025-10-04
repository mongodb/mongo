export var GeoNearRandomTest = function (name, dbToUse) {
    this.name = name;
    this.db = dbToUse || globalThis.db;
    this.t = this.db[name];
    this.reset();
    jsTest.log.info("Starting getNear test: " + name);
};

GeoNearRandomTest.prototype.reset = function reset() {
    // Reset state
    this.nPts = 0;
    this.t.drop();
    Random.srand(1234);
};

GeoNearRandomTest.prototype.mkPt = function mkPt(scale, indexBounds) {
    if (!indexBounds) {
        scale = scale || 1; // scale is good for staying away from edges
        return [(Random.rand() * 359.8 - 179.9) * scale, (Random.rand() * 180 - 90) * scale];
    } else {
        let range = indexBounds.max - indexBounds.min;
        let eps = Math.pow(2, -40);
        // Go very close to the borders but not quite there.
        return [
            Random.rand() * (range - eps) + eps + indexBounds.min,
            Random.rand() * (range - eps) + eps + indexBounds.min,
        ];
    }
};

GeoNearRandomTest.prototype.insertPts = function (nPts, indexBounds, scale, skipIndex) {
    assert.eq(this.nPts, 0, "insertPoints already called");
    this.nPts = nPts;

    let bulk = this.t.initializeUnorderedBulkOp();
    for (let i = 0; i < nPts; i++) {
        bulk.insert({_id: i, loc: this.mkPt(scale, indexBounds)});
    }
    assert.commandWorked(bulk.execute());

    if (!skipIndex) {
        if (!indexBounds) this.t.createIndex({loc: "2d"});
        else this.t.createIndex({loc: "2d"}, indexBounds);
    }
};

GeoNearRandomTest.prototype.assertIsPrefix = function (short, long, errmsg) {
    for (let i = 0; i < short.length; i++) {
        let xS = short[i] ? short[i].loc[0] : short[i].loc[0];
        let yS = short[i] ? short[i].loc[1] : short[i].loc[1];
        let dS = short[i] ? short[i].dis : 1;

        let xL = long[i] ? long[i].loc[0] : long[i].loc[0];
        let yL = long[i] ? long[i].loc[1] : long[i].loc[1];
        let dL = long[i] ? long[i].dis : 1;

        assert.eq([xS, yS, dS], [xL, yL, dL], errmsg);
    }
};

GeoNearRandomTest.prototype.testPt = function (pt, opts) {
    assert.neq(this.nPts, 0, "insertPoints not yet called");

    opts = opts || {};
    opts["sphere"] = opts["sphere"] || 0;
    opts["nToTest"] = opts["nToTest"] || this.nPts; // be careful, test is O( N^2 )

    jsTest.log.info("testing point", {pt, opts});

    let query = {loc: {}};
    query.loc[opts.sphere ? "$nearSphere" : "$near"] = pt;
    const proj = {dis: {$meta: "geoNearDistance"}};
    const runQuery = (limit) => this.t.find(query, proj).limit(limit).toArray();

    let last = runQuery(1);
    for (let i = 2; i <= opts.nToTest; i++) {
        let ret = runQuery(i);
        this.assertIsPrefix(last, ret, `Unexpected result when comparing ${i - 1} and ${i}`);

        // Make sure distances are in increasing order.
        assert.gte(ret[ret.length - 1].dis, last[last.length - 1].dis);
        last = ret;
    }

    // Test that a query using $near or $nearSphere returns the same points in order as the $geoNear
    // aggregation stage.
    const queryResults = runQuery(opts.nToTest);
    const aggResults = this.t
        .aggregate([{$geoNear: {near: pt, distanceField: "dis", spherical: opts.sphere}}, {$limit: opts.nToTest}])
        .toArray();
    assert.eq(queryResults, aggResults);
};
