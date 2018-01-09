// Contains helpers for performing geospatial calculations.

/**
 * Converts distance in degrees to radians.
 */
function deg2rad(arg) {
    return arg * Math.PI / 180.0;
}

/**
 * Converts distance in radians to degrees.
 */
function rad2deg(arg) {
    return arg * 180.0 / Math.PI;
}

/**
 * Convert a distance across the Earth's surface in meters to radians.
 */
function metersToRadians(meters) {
    const earthRadiusMeters = 6378.1 * 1000;
    return meters / earthRadiusMeters;
}
