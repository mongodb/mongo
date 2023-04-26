// Helper for generating random geo data, used in time-series tests.

function randomLongLat() {
    // Sample points uniformly on a sphere in two steps:
    // 1. Sample uniformly from a unit ball (the volume within a sphere).
    // 2. Project onto a sphere.

    for (;;) {
        const x = 2 * Random.rand() - 1;
        const y = 2 * Random.rand() - 1;
        const z = 2 * Random.rand() - 1;
        if (x * x + y * y + z * z > 1) {
            // This point is outside the unit ball: skip it.
            continue;
        }

        // Taking the [long, lat], ignoring distance from the origin, has the effect of projecting
        // onto a sphere.
        const longRadians = Math.atan2(y, x);
        const latRadians = Math.atan2(z, Math.sqrt(x * x + y * y));
        const long = longRadians * 180 / Math.PI;
        const lat = latRadians * 180 / Math.PI;
        return [long, lat];
    }
}
