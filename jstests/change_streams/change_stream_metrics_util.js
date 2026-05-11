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

// Temporarily overrides a nested TestData field at a dot-notation path with 'newValue',
// saving the original so it can be restored via 'restore()'.
export function TestDataModifyGuard(fieldPath, newValue) {
    this.path = fieldPath.split(".");
    this.pathSwap = function (newValue) {
        return this.path.reduce((obj, part, i) => {
            if (!obj || typeof obj !== "object") {
                throw new Error(
                    `could not traverse path component "${part}" because ${toJsonForLog(obj)} is not an object`,
                );
            }
            if (i === this.path.length - 1) {
                const originalValue = obj[part];
                if (newValue === undefined) {
                    delete obj[part];
                } else {
                    obj[part] = newValue;
                }
                return originalValue;
            }
            if (!obj.hasOwnProperty(part)) {
                throw new Error(`could not find property "${part}" in ${toJsonForLog(obj)}`);
            }
            return obj[part];
        }, TestData);
    };
    this.originalValue = this.pathSwap(newValue);
    this.restore = function () {
        this.pathSwap(this.originalValue);
    };
}
