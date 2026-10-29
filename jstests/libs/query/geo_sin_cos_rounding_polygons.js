/**
 * Polygons used by the sin/cos rounding recovery tests, mirroring the corpus in
 * src/mongo/db/geo/geoparser_test.cpp.
 */
// The polygons from geoparser_test.cpp that must parse on every platform once recovery is enabled.
// Vertices named B, B', B'' below have 1-ULP-adjacent longitudes that collapse to one S2Point on
// glibc 2.34+.
export const kRecoverablePolygons = [
    {
        name: "adjacent collapse [A, B, B', A]",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [0, 0],
                    [6.500000000000086, 0],
                    [6.500000000000087, 0],
                    [0, 0],
                ],
            ],
        },
    },
    {
        name: "triple adjacent collapse [A, B, B', B'', A]",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [0, 0],
                    [-31.021928199999991, 0],
                    [-31.021928199999987, 0],
                    [-31.021928199999984, 0],
                    [0, 0],
                ],
            ],
        },
    },
    {
        name: "triple non-adjacent collapse [A, B, X, Y, B', C, D, B'', A]",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [0, 0],
                    [-31.021928199999991, 0],
                    [0, 1],
                    [1, 0],
                    [-31.021928199999987, 0],
                    [2, 2],
                    [3, 3],
                    [-31.021928199999984, 0],
                    [0, 0],
                ],
            ],
        },
    },
    {
        name: "non-adjacent collapse with closing vertex [A, Z, A', A]",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [6.500000000000086, 0],
                    [0, 0],
                    [6.500000000000087, 0],
                    [6.500000000000086, 0],
                ],
            ],
        },
    },
    {
        name: "doubled closing vertex [A, B, B', A, A]",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [0, 0],
                    [6.500000000000086, 0],
                    [6.500000000000087, 0],
                    [0, 0],
                    [0, 0],
                ],
            ],
        },
    },
    {
        name: "interior ring collapse",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-1, -1],
                    [10, -1],
                    [10, 2],
                    [-1, 2],
                    [-1, -1],
                ],
                [
                    [0, 0],
                    [6.500000000000086, 0],
                    [6.500000000000087, 0],
                    [0, 0],
                ],
            ],
        },
    },
    {
        name: "multipolygon with one collapsing member",
        geo: {
            type: "MultiPolygon",
            coordinates: [
                [
                    [
                        [0, 0],
                        [6.500000000000086, 0],
                        [6.500000000000087, 0],
                        [0, 0],
                    ],
                ],
                [
                    [
                        [100, 0],
                        [101, 0],
                        [101, 1],
                        [100, 1],
                        [100, 0],
                    ],
                ],
            ],
        },
    },
    // Polygons that surfaced as a spurious "Edges .. and .. cross" validation error on glibc 2.34+,
    // in the same order as the edge cross polygons in geoparser_test.cpp.
    {
        // All 4 points share the same longitude.
        // Originally "Edges 0 and 8 cross" in a 105-vertex ring.
        name: "edge cross: collinear on a meridian",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-74.51725091699996, -52.497973255499936], // edge 0 start
                    [-74.51725091699996, -52.50424355189978], //  edge 0 end
                    [-74.51726, -52.5043], //                     synthetic: west of the meridian
                    [-74.51725091699996, -52.50343127190277], //  edge 3 start
                    [-74.51725091699996, -52.503209760749144], // edge 3 end
                    [-74.51724, -52.4979], //                     synthetic: east of the meridian
                    [-74.51725091699996, -52.497973255499936],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 1 is an endpoint of edge 0, one of
        // the pair S2 reports as crossing ("Edges 0 and 3 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-74.51725091699996, -52.50424355189978]},
    },
    {
        // B and D are non-adjacent near-duplicates, with C between them.
        // Originally "Edges 2 and 4 cross" in an 11-vertex ring.
        name: "edge cross: non-adjacent near-duplicates",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [4.811588259061592, 46.188831865509364],
                    [4.811562500735455, 46.18879804860283],
                    [4.811570979909139, 46.188809180520224],
                    [4.811562899999999, 46.188798299999966],
                    [4.811588259061592, 46.188831865509364],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 1 (B) is an endpoint of edge 0, one
        // of the pair S2 reports as crossing ("Edges 0 and 2 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [4.811562500735455, 46.18879804860283]},
    },
    {
        // B and C are adjacent near-duplicates. Originally "Edges 1 and 3 cross" in a 5-vertex
        // ring.
        name: "edge cross: adjacent near-duplicates (edges 1 and 3)",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-102.15814805857003, 20.91436701191156, 0],
                    [-102.15813803160876, 20.914376735127263, 0],
                    [-102.15812768344978, 20.91436731371167, 0],
                    [-102.15812768344978, 20.914367313711672, 0],
                    [-102.15813771041094, 20.914357590496575, 0],
                    [-102.15814805857003, 20.91436701191156, 0],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 2 (C) is an endpoint of edge 1, one
        // of the pair S2 reports as crossing ("Edges 1 and 3 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-102.15812768344978, 20.91436731371167]},
    },
    {
        // Same shape as above, but a second, independent polygon.
        name: "edge cross: adjacent near-duplicates (second polygon)",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-102.16815403780438, 20.912087153486475, 0],
                    [-102.16815449038826, 20.91208694327097, 0],
                    [-102.16816087560454, 20.91209908042594, 0],
                    [-102.16816087560454, 20.912099080425936, 0],
                    [-102.16816042302064, 20.912099290641454, 0],
                    [-102.16815403780438, 20.912087153486475, 0],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 2 (C) is an endpoint of edge 1, one
        // of the pair S2 reports as crossing ("Edges 1 and 3 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-102.16816087560454, 20.91209908042594]},
    },
    {
        name: "edge cross: wraparound near-duplicates",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-104.65965863813986, 21.426797451142136, 0],
                    [-104.65965863813986, 21.426797451142132, 0],
                    [-104.65964986786682, 21.426801469735402, 0],
                    [-104.65964350558568, 21.426789296613403, 0],
                    [-104.65965227585815, 21.426785278020446, 0],
                    [-104.65965863813986, 21.426797451142136, 0],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 1 is an endpoint of edge 1, one of
        // the pair S2 reports as crossing ("Edges 1 and 4 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-104.65965863813986, 21.426797451142132]},
    },
    {
        name: "edge cross: adjacent near-duplicates",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-104.42701666887262, 20.8220052318962, 0],
                    [-104.42700328627922, 20.82201026935565, 0],
                    [-104.42699792837841, 20.821997687140296, 0],
                    [-104.42699792837841, 20.8219976871403, 0],
                    [-104.42701131097088, 20.821992649681242, 0],
                    [-104.42701666887262, 20.8220052318962, 0],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 2 (C) is an endpoint of edge 1, one
        // of the pair S2 reports as crossing ("Edges 1 and 3 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-104.42699792837841, 20.821997687140296]},
    },
    {
        // All four named vertices share a longitude, so they are collinear on a meridian;
        // the ring retraces part of it. Synthetic vertices off the meridian, as above.
        name: "edge cross: edges 20 and 23 in original polygon",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-111.8498675, 40.5709765], //  edge 0 start
                    [-111.8498675, 40.570976], //   edge 0 end
                    [-111.849865, 40.57097575], //  synthetic: east of the meridian
                    [-111.8498675, 40.5709745], //  edge 3 start
                    [-111.8498675, 40.570974], //   edge 3 end
                    [-111.84987, 40.5709735], //    synthetic: west of the meridian
                    [-111.8498675, 40.5709765],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 1 is an endpoint of edge 0, one of
        // the pair S2 reports as crossing ("Edges 0 and 3 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-111.8498675, 40.570976]},
    },
    {
        name: "edge cross: edges 2 and 4 in original polygon",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-74.35097, 39.601508],
                    [-74.351093, 39.601508],
                    [-74.351093, 39.60166],
                    [-74.351093, 39.601364],
                    [-74.35097, 39.601508],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 1 is an endpoint of edge 0, one of
        // the pair S2 reports as crossing ("Edges 0 and 2 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-74.351093, 39.601508]},
    },
    {
        // All four named vertices share a longitude, so they are collinear on a meridian;
        // the ring retraces part of it. Synthetic vertices off the meridian, as above.
        name: "edge cross: edges 154 and 157 in original polygon",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-83.12122, 42.591314], //          edge 0 start
                    [-83.12122, 42.5913135], //         edge 0 end
                    [-83.12121, 42.591313], //          synthetic: east of the meridian
                    [-83.12122, 42.5913125], //         edge 3 start
                    [-83.12122, 42.591311999999995], // edge 3 end
                    [-83.12123, 42.5913115], //         synthetic: west of the meridian
                    [-83.12122, 42.591314],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 1 is an endpoint of edge 0, one of
        // the pair S2 reports as crossing ("Edges 0 and 3 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-83.12122, 42.5913135]},
    },
    {
        name: "edge cross: edges 1 and 12 in original polygon",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [-119.23221, 45.864018],
                    [-119.23221, 45.864043],
                    [-119.232198, 45.86402],
                    [-119.23221, 45.86402],
                    [-119.23221, 45.864018],
                ],
            ],
        },
        // A query point sitting on the suppressed edge: vertex 1 is an endpoint of edge 0, one of
        // the pair S2 reports as crossing ("Edges 0 and 2 cross") until RobustCrossingStable
        // downgrades it to a non-crossing degenerate determinant.
        queryOperand: {type: "Point", coordinates: [-119.23221, 45.864043]},
    },
];

// Polygons that are genuinely invalid, independent of any rounding difference. Recovery must never
// turn these into accepted geometry.
export const kAlwaysInvalidPolygons = [
    {
        name: "truly duplicated adjacent vertices",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [0, 0],
                    [6.5, 0],
                    [6.5, 0],
                    [0, 0],
                ],
            ],
        },
    },
    {
        name: "bowtie with genuinely crossing edges",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [0, 0],
                    [1, 1],
                    [1, 0],
                    [0, 1],
                    [0, 0],
                ],
            ],
        },
    },
    {
        name: "repeated literal coordinates at non-adjacent positions",
        geo: {
            type: "Polygon",
            coordinates: [
                [
                    [0, 0],
                    [6.500000000000086, 0],
                    [6.500000000000087, 0],
                    [6.500000000000086, 0],
                    [6.500000000000087, 0],
                    [6.500000000000086, 0],
                    [0, 0],
                ],
            ],
        },
    },
];

// Returns the names of the kRecoverablePolygons entries that this platform's libm actually makes
// degenerate, by attempting 2dsphere key generation for each one in turn. Only a subset of the
// corpus may collapse on a given platform, and on glibc < 2.34 none of them do.
//
// 'db' must be on a server running without geoRecoveryFromSinCosRoundingErrors.
export function findPolygonsRejectedWithoutRecovery(db) {
    const rejected = new Set();
    const probe = db.geo_sin_cos_platform_probe;
    for (const {name, geo} of kRecoverablePolygons) {
        probe.drop();
        assert.commandWorked(probe.createIndex({geo: "2dsphere"}));
        const res = probe.insert({_id: 0, geo: geo});
        if (res.hasWriteError()) {
            assert.eq(ErrorCodes.GeoKeyExtractionFailed, res.getWriteError().code, name);
            rejected.add(name);
        }
    }
    probe.drop();
    jsTest.log.info("Polygons this platform rejects without recovery", {
        rejected: Array.from(rejected),
    });
    return rejected;
}
