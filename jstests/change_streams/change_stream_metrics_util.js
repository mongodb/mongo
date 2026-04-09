// Helper object for retrieving change stream metrics from the 'serverStatus' command's output.
export class ServerStatusMetrics {
    static getCsCursorTotalOpened(db) {
        return this.getCsMetrics(db).cursor.totalOpened;
    }

    static getCsCursorLifespan(db) {
        return this.getCsMetrics(db).cursor.lifespan;
    }

    static getCsCursorOpenTotal(db) {
        return this.getCsMetrics(db).cursor.open.total;
    }

    static getCsCursorOpenPinned(db) {
        return this.getCsMetrics(db).cursor.open.pinned;
    }

    static getSsMetrics(db) {
        return assert.commandWorked(db.adminCommand({serverStatus: 1, metrics: 1})).metrics;
    }

    static getCsMetrics(db) {
        return this.getSsMetrics(db).changeStreams;
    }
}
