/**
 * Class that tracks the document counts in the QE state collections for every unique
 * field+value pair that exists in the encrypted data collection.
 *
 * NOTE: This tracker is only accurate if the encrypted fields being tracked all have
 * a contention factor of 0.  Also, the type of the encrypted value has to be a string.
 */
class QEStateCollectionStatsTracker {
    constructor() {
        /* fieldStats is a map of field names to a map of values mapped to objects
           containing stats counters. For example:
            {
                "first" : {
                    "erwin" : { nonAnchors: 2, anchors: 0, nullAnchor: false, ecoc: 2, new: true},
                    ...
                },
                ...
            }
        */
        this.fieldStats = {};
    }

    /**
     * Updates the stats after inserting a single encrypted document that contains the
     * specified field (key) and value pair.
     * Every insert of an encrypted field adds one non-anchor to the ESC and adds one
     * entry in the ECOC.
     *
     * @param {string} key the field name
     * @param {string} value the field value
     */
    updateStatsPostInsert(key, value) {
        if (!this.fieldStats.hasOwnProperty(key)) {
            this.fieldStats[key] = {};
        }

        const field = this.fieldStats[key];
        if (field.hasOwnProperty(value)) {
            field[value].nonAnchors++;
            field[value].ecoc++;
        } else {
            field[value] = {nonAnchors: 1, anchors: 0, nullAnchor: false, ecoc: 1, new: true};
        }
    }

    /**
     * Updates the stats after compacting the collection where documents
     * containing the specified encrypted fields exist.
     * For every encrypted value that has been inserted for each field that has not been
     * compacted/cleaned-up (i.e. has one or more ECOC entries), we update the stats for this
     * field+value pair by adding one ESC anchor, and clearing the counts for non-anchors & ecoc.
     *
     * This assumes that all non-anchors & ecoc entries for this value have been deleted after
     * compaction.
     *
     * @param {string} keys list of field names that were compacted
     */
    updateStatsPostCompactForFields(...keys) {
        keys.forEach(key => {
            if (!this.fieldStats.hasOwnProperty(key)) {
                print("Skipping field " + key +
                      " in updateStatsPostCompact because it is not tracked");
                return;
            }
            const field = this.fieldStats[key];
            Object.entries(field).forEach(([value, stats]) => {
                if (stats.ecoc > 0) {
                    stats.anchors++;
                    stats.nonAnchors = 0;
                    stats.ecoc = 0;
                }
                stats.new = false;
            });
        });
    }

    /**
     * Updates the stats after cleanup of the encrypted collection where documents
     * containing the specified encrypted fields exist.
     * For every field+value pair that has been inserted but not yet compacted/cleaned-up
     * (i.e. has one or more ECOC entries), we update the stats for this field+value pair
     * by adding one ESC null anchor (if none exists yet), and clearing the
     * counts for normal anchors, non-anchors, & ecoc.
     *
     * This assumes that all non-anchors and normal anchors for this value have been deleted
     * from the ESC after cleanup. This also assumes all ECOC entries for this value have
     * been deleted post-cleanup.
     *
     * @param {string} keys list of field names that were compacted
     */
    updateStatsPostCleanupForFields(...keys) {
        keys.forEach(key => {
            if (!this.fieldStats.hasOwnProperty(key)) {
                print("Skipping field " + key +
                      " in updateStatsPostCleanup because it is not tracked");
                return;
            }
            const field = this.fieldStats[key];
            Object.entries(field).forEach(([value, stats]) => {
                if (stats.ecoc > 0) {
                    stats.nullAnchor = true;
                    stats.nonAnchors = 0;
                    stats.anchors = 0;
                    stats.ecoc = 0;
                }
                stats.new = false;
            });
        });
    }

    /**
     * Returns an object that contains the aggregated statistics for each
     * field specified in keys.
     *
     * @param  {string} keys list of field names that were compacted
     * @returns {Object}
     */
    calculateTotalStatsForFields(...keys) {
        const totals = {
            esc: 0,                   // # of ESC entries
            escNonAnchors: 0,         // # of ESC non-anchors
            escAnchors: 0,            // # of ESC anchors
            escNullAnchors: 0,        // # of ESC null anchors
            escDeletableAnchors: 0,   // # of ESC anchors that may be deleted in the next cleanup
            escFutureNullAnchors: 0,  // # of null anchors that may be inserted in the next cleanup
            ecoc: 0,                  // # of ECOC entries
            ecocUnique: 0,            // # of ECOC entries that are unique
            new: 0,                   // # of new values
        };
        keys.forEach(key => {
            if (!this.fieldStats.hasOwnProperty(key)) {
                print("Skipping field " + key + " in stats aggregation because it is not tracked");
                return;
            }
            const field = this.fieldStats[key];
            Object.entries(field).forEach(([value, stats]) => {
                totals.esc += (stats.nonAnchors + stats.anchors + (stats.nullAnchor ? 1 : 0));
                totals.escNonAnchors += stats.nonAnchors;
                totals.escAnchors += stats.anchors;
                totals.escNullAnchors += (stats.nullAnchor ? 1 : 0);
                totals.escDeletableAnchors += ((stats.ecoc > 0) ? stats.anchors : 0);
                totals.escFutureNullAnchors += ((stats.ecoc > 0 && stats.nullAnchor == 0) ? 1 : 0);
                totals.ecoc += stats.ecoc;
                totals.ecocUnique += ((stats.ecoc > 0) ? 1 : 0);
                totals.new += (stats.new ? 1 : 0);
            });
        });

        return totals;
    }

    _calculateEstimatedEmuBinaryReads(nAnchors, nNonAnchors, hasNullAnchor, escSize) {
        let total = 0;

        // anchor binary hops
        //
        total += 1;  // null anchor read for lambda
        let rho = 2;
        if (nAnchors > 0) {
            rho = Math.pow(2, Math.floor(Math.log2(nAnchors)) + 1);
        }
        total += Math.log2(rho);           // # reads to find rho
        total += Math.log2(rho);           // # reads in the binary search iterations
        total += (nAnchors == 0 ? 1 : 0);  // extra read if no anchors exist

        // binary hops
        //
        total += (nAnchors > 0 || hasNullAnchor) ? 1 : 0;  // anchor read for lambda
        rho = Math.max(2, escSize);
        total += 1;                           // estimated # of reads to find final value of rho
        total += Math.ceil(Math.log2(rho));   // estimated # of binary search iterations
        total += (nNonAnchors == 0 ? 1 : 0);  // extra read if no non-anchors exist
        return total;
    }

    /**
     * Returns a lower-bound on how many ESC reads will be performed if a
     * cleanup is performed on the current encrypted collection state.
     * NOTE: call this *before* calling cleanup and before updating the tracker
     *       with updateStatsPostCleanupForFields.
     *
     * @param  {string} keys list of field names that have been added to the encrypted collection
     * @returns {int}
     */
    calculateEstimatedESCReadCountForCleanup(...keys) {
        let totals = this.calculateTotalStatsForFields(keys);
        let estimate = 0;

        estimate += totals.escNonAnchors;  // # of reads into in-mem delete set

        keys.forEach(key => {
            if (!this.fieldStats.hasOwnProperty(key)) {
                return;
            }
            const field = this.fieldStats[key];
            Object.entries(field).forEach(([value, stats]) => {
                if (stats.ecoc == 0) {
                    return;  // value not compacted
                }
                estimate += 1;  // null anchor read
                estimate += this._calculateEstimatedEmuBinaryReads(
                    stats.anchors, stats.nonAnchors, stats.nullAnchor, totals.esc);
            });
        });
        return estimate;
    }

    /**
     * Returns a lower-bound on how many ESC reads will be performed if a
     * compact is performed on the current encrypted collection state.
     * NOTE: call this *before* calling compact and before updating the tracker
     *       with updateStatsPostCompactForFields.
     *
     * @param  {string} keys list of field names that have been added to the encrypted collection
     * @returns {int}
     */
    calculateEstimatedESCReadCountForCompact(...keys) {
        let totals = this.calculateTotalStatsForFields(keys);
        let estimate = 0;

        estimate += totals.escNonAnchors;  // # of reads into in-mem delete set

        keys.forEach(key => {
            if (!this.fieldStats.hasOwnProperty(key)) {
                return;
            }
            const field = this.fieldStats[key];
            Object.entries(field).forEach(([value, stats]) => {
                if (stats.ecoc == 0) {
                    return;  // value not compacted
                }
                estimate += (stats.nullAnchor ? 1 : 0);  // null anchor read
                estimate += this._calculateEstimatedEmuBinaryReads(
                    stats.anchors, stats.nonAnchors, stats.nullAnchor, totals.esc);
            });
        });
        return estimate;
    }
}
