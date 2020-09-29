var CollInfos = class {
    /**
     * OO wrapper around the response of db.getCollectionInfos() to avoid calling it multiple times.
     * This class stores information about all collections but its methods typically apply to a
     * single collection, so a collName is typically required to be passed in as a parameter.
     */
    constructor(conn, connName, dbName) {
        // Special listCollections filter to prevent reloading the view catalog.
        const listCollectionsFilter = {
            $or: [
                {type: 'collection'},
                {type: {$exists: false}},
            ]
        };
        this.conn = conn;
        this.connName = connName;
        this.dbName = dbName;
        this.collInfosRes = conn.getDB(dbName).getCollectionInfos(listCollectionsFilter);
    }

    ns(collName) {
        return this.dbName + '.' + collName;
    }

    /**
     * Do additional filtering to narrow down collections that have names in collNames.
     */
    filter(desiredCollNames) {
        this.collInfosRes = this.collInfosRes.filter(info => desiredCollNames.includes(info.name));
    }

    /**
     * Get collInfo for non-capped collections.
     *
     * Don't call isCapped(), which calls listCollections.
     */
    getNonCappedCollNames() {
        const infos = this.collInfosRes.filter(info => !info.options.capped);
        return infos.map(info => info.name);
    }

    hostAndNS(collName) {
        return `${this.conn.host}--${this.ns(collName)}`;
    }

    print(collectionPrinted, collName) {
        const ns = this.ns(collName);
        const alreadyPrinted = collectionPrinted.has(this.hostAndNS(collName));

        // Extract basic collection info.
        const coll = this.conn.getDB(this.dbName).getCollection(collName);
        let collInfo = null;

        const collInfoRaw = this.collInfosRes.find(elem => elem.name === collName);
        if (collInfoRaw) {
            collInfo = {
                ns: ns,
                host: this.conn.host,
                UUID: collInfoRaw.info.uuid,
                count: coll.find().itcount()
            };
        }

        const infoPrefix = `${this.connName}(${this.conn.host}) info for ${ns} : `;
        if (collInfo !== null) {
            if (alreadyPrinted) {
                print(`${this.connName} info for ${ns} already printed. Search for ` +
                      `'${infoPrefix}'`);
            } else {
                print(infoPrefix + tojsononeline(collInfo));
            }
        } else {
            print(infoPrefix + 'collection does not exist');
        }

        const collStats = this.conn.getDB(this.dbName).runCommand({collStats: collName});
        const statsPrefix = `${this.connName}(${this.conn.host}) collStats for ${ns}: `;
        if (collStats.ok === 1) {
            if (alreadyPrinted) {
                print(`${this.connName} collStats for ${ns} already printed. Search for ` +
                      `'${statsPrefix}'`);
            } else {
                print(statsPrefix + tojsononeline(collStats));
            }
        } else {
            print(`${statsPrefix}  error: ${tojsononeline(collStats)}`);
        }

        collectionPrinted.add(this.hostAndNS(collName));

        // Return true if collInfo & collStats can be retrieved for conn.
        return collInfo !== null && collStats.ok === 1;
    }
};

var {DataConsistencyChecker} = (function() {
    "use strict";

    class PeekableCursor {
        constructor(cursor) {
            this.cursor = cursor;
            this.stashedDoc = undefined;
        }

        hasNext() {
            return this.cursor.hasNext();
        }

        peekNext() {
            if (this.stashedDoc === undefined) {
                this.stashedDoc = this.cursor.next();
            }
            return this.stashedDoc;
        }

        next() {
            const result = (this.stashedDoc === undefined) ? this.cursor.next() : this.stashedDoc;
            this.stashedDoc = undefined;
            return result;
        }
    }

    class DataConsistencyChecker {
        static getDiff(cursor1, cursor2) {
            const docsWithDifferentContents = [];
            const docsMissingOnFirst = [];
            const docsMissingOnSecond = [];

            cursor1 = new PeekableCursor(cursor1);
            cursor2 = new PeekableCursor(cursor2);

            while (cursor1.hasNext() && cursor2.hasNext()) {
                const doc1 = cursor1.peekNext();
                const doc2 = cursor2.peekNext();

                if (bsonBinaryEqual(doc1, doc2)) {
                    // The same document was found from both cursor1 and cursor2 so we just move
                    // on to the next document for both cursors.
                    cursor1.next();
                    cursor2.next();
                    continue;
                }

                const ordering = bsonWoCompare({_: doc1._id}, {_: doc2._id});
                if (ordering === 0) {
                    // The documents have the same _id but have different contents.
                    docsWithDifferentContents.push({first: doc1, second: doc2});
                    cursor1.next();
                    cursor2.next();
                } else if (ordering < 0) {
                    // The cursor1's next document has a smaller _id than the cursor2's next
                    // document. Since we are iterating the documents in ascending order by their
                    // _id, we'll never see a document with 'doc1._id' from cursor2.
                    docsMissingOnSecond.push(doc1);
                    cursor1.next();
                } else if (ordering > 0) {
                    // The cursor1's next document has a larger _id than the cursor2's next
                    // document. Since we are iterating the documents in ascending order by their
                    // _id, we'll never see a document with 'doc2._id' from cursor1.
                    docsMissingOnFirst.push(doc2);
                    cursor2.next();
                }
            }

            while (cursor1.hasNext()) {
                // We've exhausted cursor2 already, so everything remaining from cursor1 must be
                // missing from cursor2.
                docsMissingOnSecond.push(cursor1.next());
            }

            while (cursor2.hasNext()) {
                // We've exhausted cursor1 already, so everything remaining from cursor2 must be
                // missing from cursor1.
                docsMissingOnFirst.push(cursor2.next());
            }

            return {docsWithDifferentContents, docsMissingOnFirst, docsMissingOnSecond};
        }

        static dumpCollectionDiff(
            rst, collectionPrinted, primaryCollInfos, secondaryCollInfos, collName) {
            print('Dumping collection: ' + primaryCollInfos.ns(collName));

            const primaryExists = primaryCollInfos.print(collectionPrinted, collName);
            const secondaryExists = secondaryCollInfos.print(collectionPrinted, collName);

            if (!primaryExists || !secondaryExists) {
                print(`Skipping checking collection differences for ${
                    primaryCollInfos.ns(
                        collName)} since it does not exist on primary and secondary`);
                return;
            }

            const primary = primaryCollInfos.conn;
            const secondary = secondaryCollInfos.conn;

            const primarySession = primary.getDB('test').getSession();
            const secondarySession = secondary.getDB('test').getSession();
            const diff = rst.getCollectionDiffUsingSessions(
                primarySession, secondarySession, primaryCollInfos.dbName, collName);

            for (let {
                     primary: primaryDoc,
                     secondary: secondaryDoc,
                 } of diff.docsWithDifferentContents) {
                print(`Mismatching documents between the primary ${primary.host}` +
                      ` and the secondary ${secondary.host}:`);
                print('    primary:   ' + tojsononeline(primaryDoc));
                print('    secondary: ' + tojsononeline(secondaryDoc));
            }

            if (diff.docsMissingOnPrimary.length > 0) {
                print(`The following documents are missing on the primary ${primary.host}:`);
                print(diff.docsMissingOnPrimary.map(doc => tojsononeline(doc)).join('\n'));
            }

            if (diff.docsMissingOnSecondary.length > 0) {
                print(`The following documents are missing on the secondary ${secondary.host}:`);
                print(diff.docsMissingOnSecondary.map(doc => tojsononeline(doc)).join('\n'));
            }
        }
    }

    return {DataConsistencyChecker};
})();
