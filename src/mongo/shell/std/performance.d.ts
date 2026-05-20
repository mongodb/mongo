export interface PerformanceApi {
    /**
     * Returns a high-resolution, monotonic timestamp in milliseconds.
     *
     * The timestamp is relative to shell process startup and is not tied to
     * wall-clock time.
     */
    now(): number;
}

export declare const performance: PerformanceApi;
