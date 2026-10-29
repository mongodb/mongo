/**
 * Centralized names for every `admission::RateLimiter` wrapper instance.
 *
 * These mirror the `kRateLimiterName` static constants exposed on each wrapper class
 * (e.g. `IngressRequestRateLimiter::kRateLimiterName` in C++) and are used as the `limiter` field
 * of the `hangInRateLimiter` failpoint's `configureFailPoint` data so a test can force queueing on
 * exactly one limiter without parking the others.
 *
 * Keep this file in sync with the C++ constants when wrappers are added or renamed.
 */
export const RateLimiterKind = Object.freeze({
    IngressRequestRateLimiter: "ingressRequestRateLimiter",
    EgressResponseRateLimiter: "egressResponseRateLimiter",
    SessionEstablishmentRateLimiter: "SessionEstablishmentRateLimiter",
    FlowControlRateLimiter: "flowControl",
});
