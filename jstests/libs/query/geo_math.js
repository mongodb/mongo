// Contains helpers for performing geospatial calculations.

/**
 * Converts distance in degrees to radians.
 */
export function deg2rad(arg) {
    return arg * Math.PI / 180.0;
}

/**
 * Converts distance in radians to degrees.
 */
export function rad2deg(arg) {
    return arg * 180.0 / Math.PI;
}

/**
 * Convert a distance across the Earth's surface in meters to radians.
 */
export function metersToRadians(meters) {
    const earthRadiusMeters = 6378.1 * 1000;
    return meters / earthRadiusMeters;
}
