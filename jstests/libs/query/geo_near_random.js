export var GeoNearRandomTest = function(name, dbToUse) {
    this.name = name;
    this.db = (dbToUse || globalThis.db);
    this.t = this.db[name];
    this.reset();
    print("Starting getNear test: " + name);
};

GeoNearRandomTest.prototype.reset = function reset() {
    // Reset state
    this.nPts = 0;
    this.t.drop();
    Random.srand(1234);
};

GeoNearRandomTest.prototype.mkPt = function mkPt(scale, indexBounds) {
    if (!indexBounds) {
        scale = scale || 1;  // scale is good for staying away from edges
        return [((Random.rand() * 359.8) - 179.9) * scale, ((Random.rand() * 180) - 90) * scale];
    } else {
        var range = indexBounds.max - indexBounds.min;
        var eps = Math.pow(2, -40);
        // Go very close to the borders but not quite there.
        return [
            (Random.rand() * (range - eps) + eps) + indexBounds.min,
            (Random.rand() * (range - eps) + eps) + indexBounds.min
        ];
    }
};

GeoNearRandomTest.prototype.insertPts = function(nPts, indexBounds, scale, skipIndex) {
    assert.eq(this.nPts, 0, "insertPoints already called");
    this.nPts = nPts;

    var bulk = this.t.initializeUnorderedBulkOp();
    for (var i = 0; i < nPts; i++) {
        bulk.insert({_id: i, loc: this.mkPt(scale, indexBounds)});
    }
    assert.commandWorked(bulk.execute());

    if (!skipIndex) {
        if (!indexBounds)
            this.t.createIndex({loc: '2d'});
        else
            this.t.createIndex({loc: '2d'}, indexBounds);
    }
};

GeoNearRandomTest.prototype.testPt = function(pt, opts) {
    assert.neq(this.nPts, 0, "insertPoints not yet called");

    opts = opts || {};
    opts['sphere'] = opts['sphere'] || 0;
    opts['nToTest'] = opts['nToTest'] || this.nPts;  // be careful, test is O( N^2 )

    print("testing point: " + tojson(pt) + " opts: " + tojson(opts));

    let query = {loc: {}};
    query.loc[opts.sphere ? '$nearSphere' : '$near'] = pt;
    const runQuery = (proj) => this.t.find(query, proj).limit(opts.nToTest).toArray();
    const runAggregation = (distField) => {
        let geoNearSpec = {near: pt, spherical: opts.sphere};
        if (distField) {
            geoNearSpec["distanceField"] = distField;
        }
        return this.t.aggregate([{$geoNear: geoNearSpec}, {$limit: opts.nToTest}]).toArray();
    };

    // Check that find() results are in increasing order.
    const distanceProj = {dis: {$meta: "geoNearDistance"}};
    let queryResults = runQuery(distanceProj);

    assert.gte(
        queryResults.length, 2, `Expected at least 2 results, got ${queryResults.length} back.`);

    for (var i = 2; i <= opts.nToTest; i++) {
        // Make sure distances are in increasing order.
        assert.gte(queryResults[i - 1].dis,
                   queryResults[i - 2].dis,
                   `Unexpected result when comparing ${i - 1} and ${i}`);
    }

    // Test that a query using the $geoNear aggregation stage returns the same points in order as
    // find with $near or $nearSphere.
    let aggResults = runAggregation('dis');
    assert.eq(queryResults, aggResults);

    // Test that the results are the same even if we don't pass a distanceField to the aggregation
    // pipeline.
    queryResults = runQuery({} /* empty projection, no meta distance */);
    aggResults = runAggregation(/* no distanceField */);
    assert.eq(queryResults, aggResults);
};
