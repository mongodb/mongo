GeoNearRandomTest = function(name) {
    this.name = name;
    this.t = db[name];
    this.nPts = 0;

    // reset state
    this.t.drop();
    Random.srand(1234);

    print("starting test: " + name);
}


GeoNearRandomTest.prototype.mkPt = function mkPt(scale, indexBounds){
	if(!indexBounds){
		scale = scale || 1; // scale is good for staying away from edges
    	return [((Random.rand() * 359.8) - 179.9) * scale, ((Random.rand() * 180) - 90) * scale];
	}
	else{
		var range = indexBounds.max - indexBounds.min;
		var eps = Math.pow(2, -40);
		// Go very close to the borders but not quite there.
		return [( Random.rand() * (range - eps) + eps) + indexBounds.min, ( Random.rand() * (range - eps) + eps ) + indexBounds.min];
	}
    
}

GeoNearRandomTest.prototype.insertPts = function(nPts, indexBounds, scale) {
    assert.eq(this.nPts, 0, "insertPoints already called");
    this.nPts = nPts;

    for (var i=0; i<nPts; i++){
        this.t.insert({_id: i, loc: this.mkPt(scale, indexBounds)});
    }
    
    if(!indexBounds)
    	this.t.ensureIndex({loc: '2d'});
    else
    	this.t.ensureIndex({loc: '2d'}, indexBounds)
}

GeoNearRandomTest.prototype.assertIsPrefix = function(short, long) {
    for (var i=0; i < short.length; i++){
    	
    	var xS = short[i].obj ? short[i].obj.loc[0] : short[i].loc[0]
    	var yS = short[i].obj ? short[i].obj.loc[1] : short[i].loc[1]
    	var dS = short[i].obj ? short[i].dis : 1
    	
		var xL = long[i].obj ? long[i].obj.loc[0] : long[i].loc[0]
    	var yL = long[i].obj ? long[i].obj.loc[1] : long[i].loc[1]
    	var dL = long[i].obj ? long[i].dis : 1
    	
        assert.eq([xS, yS, dS], [xL, yL, dL]);
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

    
    if (!opts.sharded){
        last = last.map(function(x){return x.obj});

        var query = {loc:{}};
        query.loc[ opts.sphere ? '$nearSphere' : '$near' ] = pt;
        var near = this.t.find(query).limit(opts.nToTest).toArray();

        this.assertIsPrefix(last, near);
        assert.eq(last, near);
    }
}


