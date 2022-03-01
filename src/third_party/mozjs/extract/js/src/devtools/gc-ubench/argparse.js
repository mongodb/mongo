/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Command-line argument parser, modeled after but not identical to Python's
// argparse.

var ArgParser = class {
  constructor(desc) {
    this._params = [];
    this._doc = desc;

    this.add_argument("--help", {
      help: "display this help message",
    });
  }

  // name is '--foo', '-f', or an array of aliases.
  //
  // spec is an options object with keys:
  //   dest: key name to store the result in (optional for long options)
  //   default: value to use if not passed on command line (optional)
  //   help: description of the option to show in --help
  //   options: array of valid choices
  //
  // Prefixes of long option names are allowed. If a prefix is ambiguous, it
  // will match the first parameter added to the ArgParser.
  add_argument(name, spec) {
    const names = Array.isArray(name) ? name : [name];

    spec = Object.assign({}, spec);
    spec.aliases = names;
    for (const name of names) {
      if (!name.startsWith("-")) {
        throw new Error(`unhandled argument syntax '${name}'`);
      }
      if (name.startsWith("--")) {
        spec.dest = spec.dest || name.substr(2);
      }
      this._params.push({ name, spec });
    }
  }

  parse_args(args) {
    const opts = {};
    const rest = [];

    for (const { spec } of this._params) {
      if (spec.default !== undefined) {
        opts[spec.dest] = spec.default;
      }
    }

    const seen = new Set();
    for (let i = 0; i < args.length; i++) {
      const arg = args[i];
      if (!arg.startsWith("-")) {
        rest.push(arg);
        continue;
      } else if (arg === "--") {
        rest.push(args.slice(i+1));
        break;
      }

      if (arg == "--help" || arg == "-h") {
        this.help();
      }

      let parameter;
      let [passed, value] = arg.split("=");
      for (const { name, spec } of this._params) {
        if (passed.startsWith("--")) {
          if (name.startsWith(passed)) {
            parameter = spec;
          }
        } else if (passed.startsWith("-") && passed === name) {
          parameter = spec;
        }
        if (parameter) {
          if (value === undefined) {
            value = args[++i];
          }
          opts[parameter.dest] = value;
          break;
        }
      }

      if (parameter) {
        if (seen.has(parameter)) {
          throw new Error(`${parameter.aliases[0]} given multiple times`);
        }
        seen.add(parameter);
      } else {
        throw new Error(`invalid command-line argument '${arg}'`);
      }
    }

    for (const { name, spec } of this._params) {
      if (spec.options && !spec.options.includes(opts[spec.dest])) {
        throw new Error(`invalid ${name} value '${opts[spec.dest]}'`);
        opts[spec.dest] = spec.default;
      }
    }

    return { opts, rest };
  }

  help() {
    print(`Usage: ${this._doc}`);
    const specs = new Set(this._params.map(p => p.spec));
    const optstrs = [...specs].map(p => p.aliases.join(", "));
    let maxlen = Math.max(...optstrs.map(s => s.length));
    for (const spec of specs) {
      const name = spec.aliases[0];
      let helptext = spec.help ?? "undocumented";
      if ("options" in spec) {
        helptext += ` (one of ${spec.options.map(x => `'${x}'`).join(", ")})`;
      }
      if ("default" in spec) {
        helptext += ` (default '${spec.default}')`;
      }
      const optstr = spec.aliases.join(", ");
      print(`  ${optstr.padEnd(maxlen)}  ${helptext}`);
    }
    quit(0);
  }
};
