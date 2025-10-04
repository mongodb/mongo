import json
import os
import re
import subprocess
import sys
from collections import defaultdict, namedtuple

from sixgill import Body

scriptdir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

HazardSummary = namedtuple(
    "HazardSummary", ["function", "variable", "type", "GCFunction", "location"]
)

Callgraph = namedtuple(
    "Callgraph",
    [
        "functionNames",
        "nameToId",
        "mangledToUnmangled",
        "unmangledToMangled",
        "calleesOf",
        "callersOf",
        "tags",
        "calleeGraph",
        "callerGraph",
    ],
)


def equal(got, expected):
    if got != expected:
        print("Got '%s', expected '%s'" % (got, expected))


def extract_unmangled(func):
    return func.split("$")[-1]


class Test(object):
    def __init__(self, indir, outdir, cfg, verbose=0):
        self.indir = indir
        self.outdir = outdir
        self.cfg = cfg
        self.verbose = verbose

    def infile(self, path):
        return os.path.join(self.indir, path)

    def binpath(self, prog):
        return os.path.join(self.cfg.sixgill_bin, prog)

    def compile(self, source, options=""):
        env = os.environ
        env["CCACHE_DISABLE"] = "1"
        if "-fexceptions" not in options and "-fno-exceptions" not in options:
            options += " -fno-exceptions"
        cmd = "{CXX} -c {source} -O3 -std=c++17 -fplugin={sixgill} -fplugin-arg-xgill-mangle=1 {options}".format(  # NOQA: E501
            source=self.infile(source),
            CXX=self.cfg.cxx,
            sixgill=self.cfg.sixgill_plugin,
            options=options,
        )
        if self.cfg.verbose > 0:
            print("Running %s" % cmd)
        subprocess.check_call(["sh", "-c", cmd])

    def load_db_entry(self, dbname, pattern):
        """Look up an entry from an XDB database file, 'pattern' may be an exact
        matching string, or an re pattern object matching a single entry."""

        if hasattr(pattern, "match"):
            output = subprocess.check_output(
                [self.binpath("xdbkeys"), dbname + ".xdb"], universal_newlines=True
            )
            matches = list(filter(lambda _: re.search(pattern, _), output.splitlines()))
            if len(matches) == 0:
                raise Exception("entry not found")
            if len(matches) > 1:
                raise Exception("multiple entries found")
            pattern = matches[0]

        output = subprocess.check_output(
            [self.binpath("xdbfind"), "-json", dbname + ".xdb", pattern],
            universal_newlines=True,
        )
        return json.loads(output)

    def run_analysis_script(self, startPhase="gcTypes", upto=None):
        open("defaults.py", "w").write(
            """\
analysis_scriptdir = '{scriptdir}'
sixgill_bin = '{bindir}'
""".format(
                scriptdir=scriptdir, bindir=self.cfg.sixgill_bin
            )
        )
        cmd = [
            sys.executable,
            os.path.join(scriptdir, "analyze.py"),
            ["-q", "", "-v"][min(self.verbose, 2)],
        ]
        cmd += ["--first", startPhase]
        if upto:
            cmd += ["--last", upto]
        cmd.append("--source=%s" % self.indir)
        cmd.append("--js=%s" % self.cfg.js)
        if self.cfg.verbose:
            print("Running " + " ".join(cmd))
        subprocess.check_call(cmd)

    def computeGCTypes(self):
        self.run_analysis_script("gcTypes", upto="gcTypes")

    def computeHazards(self):
        self.run_analysis_script("gcTypes")

    def load_text_file(self, filename, extract=lambda l: l):
        fullpath = os.path.join(self.outdir, filename)
        values = (extract(line.strip()) for line in open(fullpath, "r"))
        return list(filter(lambda _: _ is not None, values))

    def load_json_file(self, filename, reviver=None):
        fullpath = os.path.join(self.outdir, filename)
        with open(fullpath) as fh:
            return json.load(fh, object_hook=reviver)

    def load_gcTypes(self):
        def grab_type(line):
            m = re.match(r"^(GC\w+): (.*)", line)
            if m:
                return (m.group(1) + "s", m.group(2))
            return None

        gctypes = defaultdict(list)
        for collection, typename in self.load_text_file(
            "gcTypes.txt", extract=grab_type
        ):
            gctypes[collection].append(typename)
        return gctypes

    def load_typeInfo(self, filename="typeInfo.txt"):
        return self.load_json_file(filename)

    def load_funcInfo(self, filename="limitedFunctions.lst"):
        return self.load_json_file(filename)

    def load_gcFunctions(self):
        return self.load_text_file("gcFunctions.lst", extract=extract_unmangled)

    def load_callgraph(self):
        data = Callgraph(
            functionNames=["dummy"],
            nameToId={},
            mangledToUnmangled={},
            unmangledToMangled={},
            calleesOf=defaultdict(list),
            callersOf=defaultdict(list),
            tags=defaultdict(set),
            calleeGraph=defaultdict(dict),
            callerGraph=defaultdict(dict),
        )

        def lookup(id):
            mangled = data.functionNames[int(id)]
            return data.mangledToUnmangled.get(mangled, mangled)

        def add_call(caller, callee, limit):
            data.calleesOf[caller].append(callee)
            data.callersOf[callee].append(caller)
            data.calleeGraph[caller][callee] = True
            data.callerGraph[callee][caller] = True

        def process(line):
            if line.startswith("#"):
                name = line.split(" ", 1)[1]
                data.nameToId[name] = len(data.functionNames)
                data.functionNames.append(name)
                return

            if line.startswith("="):
                m = re.match(r"^= (\d+) (.*)", line)
                mangled = data.functionNames[int(m.group(1))]
                unmangled = m.group(2)
                data.nameToId[unmangled] = id
                data.mangledToUnmangled[mangled] = unmangled
                data.unmangledToMangled[unmangled] = mangled
                return

            # Sample lines:
            #   D 10 20
            #   D /3 10 20
            #   D 3:3 10 20
            # All of these mean that there is a direct call from function #10
            # to function #20. The latter two mean that the call is made in a
            # context where the 0x1 and 0x2 properties (3 == 0x1 | 0x2) are in
            # effect. The `/n` syntax was the original, which was then expanded
            # to `m:n` to allow multiple calls to be combined together when not
            # all calls have the same properties in effect. The `/n` syntax is
            # deprecated.
            #
            # The properties usually refer to "limits", eg "GC is suppressed
            # in the scope surrounding this call". For testing purposes, the
            # difference between `m` and `n` in `m:n` is currently ignored.
            tokens = line.split(" ")
            limit = 0
            if tokens[1].startswith("/"):
                attr_str = tokens.pop(1)
                limit = int(attr_str[1:])
            elif ":" in tokens[1]:
                attr_str = tokens.pop(1)
                limit = int(attr_str[0 : attr_str.index(":")])

            if tokens[0] in ("D", "R"):
                _, caller, callee = tokens
                add_call(lookup(caller), lookup(callee), limit)
            elif tokens[0] == "T":
                data.tags[tokens[1]].add(line.split(" ", 2)[2])
            elif tokens[0] in ("F", "V"):
                pass

            elif tokens[0] == "I":
                m = re.match(r"^I (\d+) VARIABLE ([^\,]*)", line)
                pass

        self.load_text_file("callgraph.txt", extract=process)
        return data

    def load_hazards(self):
        def grab_hazard(line):
            m = re.match(
                r"Function '(.*?)' has unrooted '(.*?)' of type '(.*?)' live across GC call '(.*?)' at (.*)",  # NOQA: E501
                line,
            )
            if m:
                info = list(m.groups())
                info[0] = info[0].split("$")[-1]
                info[3] = info[3].split("$")[-1]
                return HazardSummary(*info)
            return None

        return self.load_text_file("hazards.txt", extract=grab_hazard)

    def process_body(self, body):
        return Body(body)

    def process_bodies(self, bodies):
        return [self.process_body(b) for b in bodies]
