/*
 * Arbitraries for generating GeoJSON metrics.
 */

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {normalDistRealArb} from "jstests/write_path/timeseries/pbt/lib/arb_utils.js";

const defaultLatitudeMin = -90;
const defaultLatitudeMax = 90;
const defaultLongitudeMin = -180;
const defaultLongitudeMax = 180;

/**
 * Make a GeoData Point arbitrary.
 * @param {Object} [ranges] ranges for longitude and latitude
 * @param {Range} [ranges.longitudeRange]
 * @param {Range} [ranges.latitudeRange]
 *
 * @returns {fc.Arbitrary<Object>} an arbitrary to generate a GeoJSON point
 */
export function makeLongLatArb(ranges = {}) {
    const longitudeRange = ranges?.longitudeRange ?? {min: defaultLongitudeMin, max: defaultLongitudeMax};
    const latitudeRange = ranges?.latitudeRange ?? {min: defaultLatitudeMin, max: defaultLatitudeMax};
    return fc.tuple(
        normalDistRealArb(longitudeRange.min, longitudeRange.max),
        normalDistRealArb(latitudeRange.min, latitudeRange.max),
    );
}

/**
 * Make a GeoData Point arbitrary.
 * @param {Object} [ranges] ranges for longitude and latitude
 * @param {Range} [ranges.latitudeRange]
 * @param {Range} [ranges.longitudeRange]
 *
 * @returns {fc.Arbitrary<Object>} an arbitrary to generate a GeoJSON point
 */
export function makeGeoPointArb(ranges = {}) {
    return makeLongLatArb(ranges).map(([long, lat]) => {
        return {type: "Point", coordinates: [long, lat]};
    });
}

/**
 * Factory to make a GeoData Point arbitrary.
 * @param {function} [prototype]
 * @param {Object} [options] options to forward to the prototype
 *
 * @returns {fc.Arbitrary<Object>} an arbitrary to generate a GeoJSON point
 */
export function makeGeoArbFactory(prototype, options = {}) {
    return function () {
        return prototype(options);
    };
}

/**
 * Make an arbitrary that generates a $geoNear aggregation pipeline using spherical distance
 * (equivalent to $nearSphere). Works with both timeseries and regular collections.
 *
 * @param {string} geoField geospatial data field (used as the index key)
 * @param {number} [maxDistanceMeters] maximum distance in meters
 * @param {number} [minDistanceMeters] minimum distance in meters
 * @param {Object} [ranges]
 * @param {Range} [ranges.latitudeRange]
 * @param {Range} [ranges.longitudeRange]
 * @returns {fc.Arbitrary<Array>} an arbitrary that generates a $geoNear aggregation pipeline
 */
export function makeGeospatialAggregationPipelineArb(
    geoField,
    maxDistanceMeters = 45_000_000,
    minDistanceMeters = 0.0,
    ranges = {},
) {
    const radiusArb = normalDistRealArb(minDistanceMeters, maxDistanceMeters);
    const longLatArb = makeLongLatArb(ranges);
    return fc.tuple(radiusArb, longLatArb).map(([maxDist, [long, lat]]) => {
        return [
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [long, lat]},
                    distanceField: "dist",
                    maxDistance: maxDist,
                    key: geoField,
                    spherical: true,
                },
            },
        ];
    });
}
