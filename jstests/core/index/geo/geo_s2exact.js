// Queries on exact geometry should return the exact geometry.
// @tags: [
//   requires_getmore,
// ]

let t = db.geo_s2exact;
t.drop();

function test(geometry) {
    t.insert({geo: geometry});
    assert.eq(1, t.find({geo: geometry}).itcount(), tojson(geometry));
    t.createIndex({geo: "2dsphere"});
    assert.eq(1, t.find({geo: geometry}).itcount(), tojson(geometry));
    t.dropIndex({geo: "2dsphere"});
}

let pointA = {"type": "Point", "coordinates": [40, 5]};
test(pointA);

let someline = {
    "type": "LineString",
    "coordinates": [
        [40, 5],
        [41, 6],
    ],
};
test(someline);

let somepoly = {
    "type": "Polygon",
    "coordinates": [
        [
            [40, 5],
            [40, 6],
            [41, 6],
            [41, 5],
            [40, 5],
        ],
    ],
};
test(somepoly);
