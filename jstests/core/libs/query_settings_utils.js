/**
 * Utility class for testing query settings.
 */
export class QuerySettingsUtils {
    /**
     * Create a query settings utility class.
     */
    constructor(db, coll) {
        this.db = db;
        this.coll = coll;
    }

    makeQueryInstance(filter = {}) {
        return {find: this.coll.getName(), $db: this.db.getName(), filter};
    }
}
