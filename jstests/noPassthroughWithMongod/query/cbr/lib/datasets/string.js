/*
   A dataset and predicates over varied strings
*/

export class StringDataset {
    docs() {
        const string_docs = [];
        // Insert each printable character once
        for (let i = 32; i < 127; i++) {
            string_docs.push({a: String.fromCharCode(i)});
        }

        // Insert certain special strings
        for (let i = 0; i < 1000; i++) {
            string_docs.push({a: ''});
            string_docs.push({a: ' '});
            // Strings of various lengths
            string_docs.push({a: 'm'.repeat(i)});
            // Test case-insensitivity
            string_docs.push({a: 'K'});
            string_docs.push({a: 'k'});
            // Test non-ASCII handling
            // TODO(SERVER-100515): string_docs.push({a: 'Алабала'});
        }

        return string_docs;
    }

    predicates() {
        let predicates = [
            {a: ''},
            {a: {$ne: ''}},
            {a: {$gt: ''}},
            {a: {$lt: ''}},

            {a: ' '},
            {a: {$ne: ' '}},
            {a: {$gt: ' '}},
            {a: {$lt: ' '}},

            {a: 'K'},
            {a: 'k'},
            {a: null},
            {a: 'Алабала'},
            {a: {$gt: 'Н'}},
            {a: {$lt: 'Н'}},

            {a: {$type: 'string'}},
        ];
        for (const str of ['l',
                           'm',
                           'n',
                           'ml',
                           'mm',
                           'mn',
                           'mmmmmmmmmml',
                           'mmmmmmmmmmm',
                           'mmmmmmmmmmn',
                           'mmmmmmmmmmmmmmmmmmmmml',
                           'mmmmmmmmmmmmmmmmmmmmmm',
                           'mmmmmmmmmmmmmmmmmmmmmn']) {
            predicates.push({a: str});
            // TODO(SERVER-99093): predicates.push({a: {$lt: str}});
            // TODO(SERVER-99093): predicates.push({a: {$gte: str}});
            // TODO(SERVER-99093): predicates.push({a: {$ne: str}});
        }

        return predicates;
    }
}
