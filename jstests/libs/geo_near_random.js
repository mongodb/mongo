GeoNearRandomTest = function(name) {
    this.name = name;
    this.t = db[name];
    this.nPts = 0;

    // reset state
    this.t.drop();
    Random.srand(1234);

    print("starting test: " + name);
}


GeoNearRandomTest.prototype.mkPt = function mkPt(scale){
    scale = scale || 1; // scale is good for staying away from edges
    return [((Random.rand() * 359.8) - 179.9) * scale, ((Random.rand() * 180) - 90) * scale];
}

GeoNearRandomTest.prototype.insertPts = function(nPts) {
    assert.eq(this.nPts, 0, "insertPoints already called");
    this.nPts = nPts;

    for (var i=0; i<nPts; i++){
        this.t.insert({_id: i, loc: this.mkPt()});
    }

    this.t.ensureIndex({loc: '2d'});
}

GeoNearRandomTest.prototype.assertIsPrefix = function(short, long) {
    for (var i=0; i < short.length; i++){
        assert.eq(short[i], long[i]);
    }
} 

GeoNearRandomTest.prototype.testPt = function(pt, opts) {
    assert.neq(this.nPts, 0, "insertPoints not yet called");

    opts = opts || {};
    opts['sphere'] = opts['sphere'] || 0;
    opts['nToTest'] = opts['nToTest'] || this.nPts; // be careful, test is O( N^2 )

    print("testing point: " + tojson(pt) + " opts: " + tojson(opts));


    var cmd = {geoNear:this.t.getName(), near: pt, num: 1, spherical:opts.sphere};

    var last = db.runCommand(cmd).results;
    for (var i=2; i <= opts.nToTest; i++){
        //print(i); // uncomment to watch status
        cmd.num = i
        var ret = db.runCommand(cmd).results;

        try {
            this.assertIsPrefix(last, ret);
        } catch (e) {
            print("*** failed while compairing " + (i-1) + " and " + i);
            printjson(cmd);
            throw e; // rethrow
        }

        last = ret;
    }

    
    last = last.map(function(x){return x.obj});

    var query = {loc:{}};
    query.loc[ opts.sphere ? '$nearSphere' : '$near' ] = pt;
    var near = this.t.find(query).limit(opts.nToTest).toArray();

    this.assertIsPrefix(last, near);
    assert.eq(last, near);
}


