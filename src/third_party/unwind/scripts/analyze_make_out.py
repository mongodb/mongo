#!/usr/bin/env python3

import sys
import re
import json

class State:
    def __init__(self):
        self.dir_stack = []
        self.srcs = {}
        self.libs = []

    def normalize_src(self, src):
        return re.sub(r"(.*)/unwind/dist/src/", "dist/src/", src)

    def add_source(self, src, obj=None):
        self.srcs[obj] = {'src':src}

    def add_library(self, lib, deps=[]):
        self.libs.append({'name':lib, 'deps':deps})

    def _analyze_lib_deps(self, deps):
        parts = deps.split(' ')
        out = []
        i = 0
        while i < len(parts):
            p = parts[i]
            if p == "":
                pass
            elif p == "-rpath":
                i += 1  # eat rpath and its argument
            elif re.match(r"-l(.*)", p):
                pass
            else:
                out.append(p)
            i += 1
        return out

    def dbg(self, s):
        print(s, file=sys.stderr)

    def load(self, f):
        for line in f:
            line = line.rstrip()
            # self.dbg(f"IN: {line}")
            if m := re.match(r"make(?:\[(\d*)\])?: Entering directory '(.*)'.*", line):
                level, dir = m[1] or 0, m[2]
                self.dbg(f"[Entering] Level: {level}, Dir: \"{dir}\"")
                self.dir_stack.append(dir)
                continue
            elif m := re.match(r"make(?:\[(\d*)\])?: Leaving directory '(.*)'.*", line):
                level, dir = m[1] or 0, m[2]
                self.dbg(f"[Leaving] Level: {level}, Dir: \"{dir}\"")
                self.dir_stack.pop()
                continue
            elif m:= re.match(r"\S+\s+\.\./libtool\s+--tag=(\S+)\s+--mode=(\S+)\s+(.*)", line):
                tag, mode, line = m.groups()
                # self.dbg(f"[libtool] Tag: {tag}, Mode: {mode}, Tail: \"{line}\"")
                if mode == "compile":
                    if m := re.match(r".*\s+-o\s+(\S+)\s+(\S+)$", line):
                        obj, src = m.groups()
                        src = self.normalize_src(src)
                        self.dbg(f"[compile] Src: \"{src}\", Obj: \"{obj}\"")
                        self.add_source(src, obj=obj)
                    continue
                elif mode == "link":
                    if m := re.match(r".*\s+-o\s+(\S+)\s+(.*)", line):
                        lib, deps = m.groups()
                        deps = self._analyze_lib_deps(deps)
                        def lib_strip(x):
                            return re.sub(r"lib(.*).la", r"\1", x)
                        lib = lib_strip(lib)
                        deps = [lib_strip(d) for d in deps]
                        self.dbg(f"[link] Lib: \"{lib}\", Deps: \"{deps}\"")
                        self.add_library(lib, deps=deps)
                    continue

    def dump(self):
        doc = []
        for lib in self.libs:
            srcs = []
            deps = []
            for dep in lib["deps"]:
                if r := self.srcs.get(dep):
                    srcs.append(r["src"])
                else:
                    deps.append(dep)
            doc.append({"name": lib["name"], "srcs": srcs, "deps": deps})

        print(json.dumps(doc, indent=4))

if __name__ == "__main__":
    state = State()
    state.load(sys.stdin)
    state.dump()
