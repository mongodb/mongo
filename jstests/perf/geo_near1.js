var t = db.bench.geo_near1;
t.drop()

var numPts = 1000*1000;


for (var i=0; i < numPts; i++){
    x = (Math.random() * 100) - 50;
    y = (Math.random() * 100) - 50;
    t.insert({loc: [x,y], i: i});
}
