'use strict';

/**
 * indexed_insert_text.js
 *
 * Inserts some documents into a collection with a text index.
 */
var $config = (function() {

    var states = {
        init: function init(db, collName) {
            // noop
            // other workloads that extend this workload use this method
        },

        insert: function insert(db, collName) {
            var doc = {};
            var snippet = this.getRandomTextSnippet();
            doc[this.indexedField] = snippet;
            var res = db[collName].insert(doc);
            assertAlways.writeOK(res);
            assertAlways.eq(1, res.nInserted, tojson(res));
            // TODO: what else can we assert? should that go in a read test?

            // Searching for the text we inserted should return at least one doc.
            // It might also return docs inserted by other threads, but it should always return
            // something.
            if (Array.isArray(snippet)) {
                snippet = snippet.join(' ');
            }
            assertWhenOwnColl.gt(db[collName].find({$text: {$search: snippet}}).itcount(), 0);
        }
    };

    var transitions = {init: {insert: 1}, insert: {insert: 1}};

    function setup(db, collName, cluster) {
        var ixSpec = {};
        ixSpec[this.indexedField] = 'text';
        // Only allowed to create one text index, other tests may create one.
        assertWhenOwnColl.commandWorked(db[collName].ensureIndex(ixSpec));
    }

    var text = [
        'We’re truly excited to announce the availability of the first MongoDB',
        '2.8 release candidate (rc0), headlined by improved concurrency (including',
        'document-level locking), compression, and pluggable storage engines.',

        'We’ve put the release through extensive testing, and will be hard at work in',
        'the coming weeks optimizing and tuning some of the new features. Now it’s',
        'your turn to help ensure the quality of this important release. Over the',
        'next three weeks, we challenge you to test and uncover any lingering issues',
        'by participating in our MongoDB 2.8 Bug Hunt. Winners are entitled to some',
        'great prizes (details below).  MongoDB 2.8 RC0',

        'In future posts we’ll share more information about all the features that',
        'make up the 2.8 release. We will begin today with our three headliners:',

        'Pluggable Storage Engines',

        'The new pluggable storage API allows external parties to build custom storage',
        'engines that seamlessly integrate with MongoDB. This opens the door for the',
        'MongoDB Community to develop a wide array of storage engines designed for',
        'specific workloads, hardware optimizations, or deployment architectures.',

        'Pluggable storage engines are first-class players in the MongoDB',
        'ecosystem. MongoDB 2.8 ships with two storage engines, both of which',
        'use the pluggable storage API. Our original storage engine, now named',
        '“MMAPv1”, remains as the default. We are also introducing a new',
        'storage engine, WiredTiger, that fulfills our desire to make MongoDB',
        'burn through write-heavy workloads and be more resource efficient.',

        'WiredTiger was created by the lead engineers of Berkeley DB and',
        'achieves high concurrency and low latency by taking full advantage',
        'of modern, multi-core servers with access to large amounts of',
        'RAM. To minimize on-disk overhead and I/O, WiredTiger uses compact',
        'file formats, and optionally, compression. WiredTiger is key to',
        'delivering the other two features we’re highlighting today.',
        'Improved Concurrency',

        'MongoDB 2.8 includes significant improvements to concurrency, resulting',
        'in greater utilization of available hardware resources, and vastly better',
        'throughput for write-heavy workloads, including those that mix reading',
        'and writing.',

        'Prior to 2.8, MongoDB’s concurrency model supported database',
        'level locking. MongoDB 2.8 introduces document-level locking with',
        'the new WiredTiger storage engine, and brings collection-level',
        'locking to MMAPv1. As a result, concurrency will improve for all',
        'workloads with a simple version upgrade. For highly concurrent',
        'use cases, where writing makes up a significant portion of',
        'operations, migrating to the WiredTiger storage engine will',
        'dramatically improve throughput and performance.',

        'The improved concurrency also means that MongoDB will more',
        'fully utilize available hardware resources. So whereas CPU',
        'usage in MongoDB has been traditionally fairly low, it will',
        'now correspond more directly to system throughput.',
        'Compression',

        'The WiredTiger storage engine in MongoDB 2.8 provides',
        'on-disk compression, reducing disk I/O and storage footprint by',
        '30-80%. Compression is configured individually for each collection and',
        'index, so users can choose the compression algorithm most appropriate',
        'for their data. In 2.8, WiredTiger compression defaults to Snappy',
        'compression, which provides a good compromise between speed and',
        'compression rates. For greater compression, at the cost of additional',
        'CPU utilization, you can switch to zlib compression.',

        'For more information, including how to seamlessly upgrade',
        'to the WiredTiger storage engine, please see the 2.8 Release',
        'Notes.'
    ];

    return {
        threadCount: 20,
        iterations: 20,
        states: states,
        transitions: transitions,
        data: {
            indexedField: 'indexed_insert_text',
            getRandomTextSnippet: function getRandomTextSnippet() {
                return this.text[Random.randInt(this.text.length)];
            },
            text: text
        },
        setup: setup
    };

})();
