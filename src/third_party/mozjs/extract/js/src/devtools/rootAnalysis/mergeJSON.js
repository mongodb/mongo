/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */

var infiles = [...scriptArgs];
var outfile = infiles.pop();

let output;
for (const filename of infiles) {
    const data = JSON.parse(os.file.readFile(filename));
    if (!output) {
        output = data;
    } else if (Array.isArray(data) != Array.isArray(output)) {
        throw new Error('mismatched types');
    } else if (Array.isArray(output)) {
        output.push(...data);
    } else {
        Object.assign(output, data);
    }
}

var origOut = os.file.redirect(outfile);
print(JSON.stringify(output, null, 4));
os.file.close(os.file.redirect(origOut));
