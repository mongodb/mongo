/*
 * Arbitraries for generating GeoJSON metrics.
 */

import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const defaultLatitudeMin = -90.0;
const defaultLatitudeMax = 90.0;
const defaultLongitudeMin = -180.0;
const defaultLongitudeMax = 180.0;

const doubleArbOpts = {
    minExcluded: false,
    maxExcluded: false,
    noNaN: true,
    noDefaultInfinity: false,
};

/**
 * Make a GeoData Point arbitrary.
 * @param {Object} [ranges] ranges for longitude and latitude
 * @param {Range} [ranges.latitudeRange]
 * @param {Range} [ranges.longitudeRange]
 *
 * @returns {fc.Arbitrary<Object>} an arbitrary to generate a GeoJSON point
 */
export function makeLongLatArb(ranges = {}) {
    const latitudeRange = ranges?.latitudeRange ?? {min: defaultLatitudeMin, max: defaultLatitudeMax};
    const longitudeRange = ranges?.longitudeRange ?? {min: defaultLongitudeMin, max: defaultLongitudeMax};
    return fc.tuple(
        fc.double({min: longitudeRange.min, max: longitudeRange.max, ...doubleArbOpts}),
        fc.double({min: latitudeRange.min, max: latitudeRange.max, ...doubleArbOpts}),
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
    return fc.record({
        type: fc.constant("Point"),
        coordinates: makeLongLatArb(ranges),
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
 *
 * @param {string} geoField geospatial data field
 * @param {number | undefined} [maxDistanceMeters]
 * @param {number} [minDistanceMeters]
 * @param {Object} [ranges]
 * @param {Range} [ranges.latitudeRange]
 * @param {Range} [ranges.longitudeRange]
 * @returns {fc.Arbitrary<Object>} an arbitrary to generate a geoWithin query
 */
export function makeGeospatialQueryArb(geoField, maxDistanceMeters = undefined, minDistanceMeters = 0.0, ranges = {}) {
    return fc.record({
        [geoField]: fc.record({
            "$geoWithin": fc.record({
                "$centerSphere": fc.tuple(
                    makeLongLatArb(ranges),
                    fc.double({min: minDistanceMeters, max: maxDistanceMeters, noNaN: true}),
                ),
            }),
        }),
    });
}
