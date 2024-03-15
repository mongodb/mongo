/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/* -*- indent-tabs-mode: nil; js-indent-level: 4 -*- */
"use strict";

loadRelativeToScript('utility.js');
loadRelativeToScript('annotations.js');
loadRelativeToScript('loadCallgraph.js');

function usage() {
  throw "Usage: computeGCFunctions.js <rawcalls1.txt> <rawcalls2.txt>... --outputs <out:callgraph.txt> <out:gcFunctions.txt> <out:gcFunctions.lst> <out:gcEdges.txt> <out:limitedFunctions.lst>";
}

if (typeof scriptArgs[0] != 'string')
  usage();

var start = "Time: " + new Date;

try {
  var options = parse_options([
    {
      name: '--verbose',
      type: 'bool'
    },
    {
      name: 'inputs',
      dest: 'rawcalls_filenames',
      nargs: '+'
    },
    {
      name: '--outputs',
      type: 'bool'
    },
    {
      name: 'callgraph',
      type: 'string',
      default: 'callgraph.txt'
    },
    {
      name: 'gcFunctions',
      type: 'string',
      default: 'gcFunctions.txt'
    },
    {
      name: 'gcFunctionsList',
      type: 'string',
      default: 'gcFunctions.lst'
    },
    {
      name: 'limitedFunctions',
      type: 'string',
      default: 'limitedFunctions.lst'
    },
  ]);
} catch {
  printErr("Usage: computeGCFunctions.js [--verbose] <rawcalls1.txt> <rawcalls2.txt>... --outputs <out:callgraph.txt> <out:gcFunctions.txt> <out:gcFunctions.lst> <out:gcEdges.txt> <out:limitedFunctions.lst>");
  quit(1);
};

function info(message) {
  if (options.verbose) {
    printErr(message);
  }
}

var {
  gcFunctions,
  functions,
  calleesOf,
  limitedFunctions
} = loadCallgraph(options.rawcalls_filenames, options.verbose);

info("Writing " + options.gcFunctions);
redirect(options.gcFunctions);

for (var name in gcFunctions) {
    for (let readable of (functions.readableName[name] || [name])) {
        print("");
        const fullname = (name == readable) ? name : name + "$" + readable;
        print("GC Function: " + fullname);
        let current = name;
        do {
            current = gcFunctions[current];
            if (current === 'internal')
                ; // Hit the end
            else if (current in functions.readableName)
                print("    " + functions.readableName[current][0]);
            else
                print("    " + current);
        } while (current in gcFunctions);
    }
}

info("Writing " + options.gcFunctionsList);
redirect(options.gcFunctionsList);
for (var name in gcFunctions) {
    if (name in functions.readableName) {
        for (var readable of functions.readableName[name])
            print(name + "$" + readable);
    } else {
        print(name);
    }
}

info("Writing " + options.limitedFunctions);
redirect(options.limitedFunctions);
print(JSON.stringify(limitedFunctions, null, 4));

info("Writing " + options.callgraph);
redirect(options.callgraph);
saveCallgraph(functions, calleesOf);
