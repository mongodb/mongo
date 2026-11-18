/**
 * Deduplication for the FSM change-stream reader.
 *
 * A retried sharding notification (shardsvrNotifyShardingEvent) can surface the same command
 * (DDL) change event twice, identical except clusterTime/_id/wallTime. This tracker lets the
 * reader collapse such duplicates at read time so it reads exactly one event per logical
 * operation, and downstream counting/matching sees a clean sequence. Pure JS (no server
 * dependency).
 */

/**
 * Builds a stable, per-operation dedup key for command (DDL) change events, or null when the
 * event must never be deduplicated. Both keys use a UUID minted once per collection/operation
 * incarnation, so it is identical across the duplicate copies of one retried notification yet
 * distinct across legitimate operations:
 *
 * - reshardCollection (also emitted by moveCollection/unshardCollection): keyed by
 *   `ns | operationDescription.reshardUUID` — the reshard operation's own UUID.
 * - shardCollection: keyed by `ns | uuid` — the collection UUID, present in the event when
 *   showExpandedEvents is on. shard -> unshard -> shard never collapses: unsharding commits a
 *   brand-new collection, so each shardCollection incarnation carries a fresh UUID.
 *
 * @param {Object} event - A change stream event.
 * @returns {string|null}
 */
function makeCommandDedupKey(event) {
    if (!event || !event.ns || !event.ns.db || !event.ns.coll) {
        return null;
    }
    if (event.operationType === "reshardCollection") {
        if (
            !event.hasOwnProperty("operationDescription") ||
            event.operationDescription.reshardUUID === undefined
        ) {
            return null;
        }
        return `${event.ns.db}.${event.ns.coll}|reshard|${tojson(
            event.operationDescription.reshardUUID,
        )}`;
    }
    if (event.operationType === "shardCollection") {
        if (event.uuid === undefined) {
            return null;
        }
        return `${event.ns.db}.${event.ns.coll}|shard|${tojson(event.uuid)}`;
    }
    return null;
}

/**
 * Tracks which command (DDL) events a reader has already emitted, so a subsequently observed
 * duplicate copy is collapsed (not recorded/counted).
 */
class DedupCommandEventTracker {
    /**
     * @param {Array<string>} [seedKeys] - Dedup keys already emitted before the reader's
     *     start point, so it collapses a duplicate whose first copy precedes the start.
     */
    constructor(seedKeys = []) {
        this._seenKeys = new Set(seedKeys);
        this._duplicateCount = 0;
    }

    /**
     * Returns true when `event` is a duplicate command event that should be collapsed.
     * @param {Object} event - A change stream event.
     * @returns {boolean}
     */
    shouldSkip(event) {
        const key = makeCommandDedupKey(event);
        if (key === null) {
            return false;
        }
        if (this._seenKeys.has(key)) {
            this._duplicateCount++;
            return true;
        }
        this._seenKeys.add(key);
        return false;
    }

    /**
     * Number of duplicate command events collapsed so far.
     * @returns {number}
     */
    getDuplicateCount() {
        return this._duplicateCount;
    }
}

export {makeCommandDedupKey, DedupCommandEventTracker};
