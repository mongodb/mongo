#!/usr/bin/env python3

""" Convert tcmalloc's Bazel filetree to SConscript

This sets up an environment in which Bazel's Python code
can execute and produce equivalent Scons directives.

Supports the subset of Bazel that TCMalloc needs.
"""

import copy
import glob
import json
import os
import re
import sys
import textwrap
    
############################################################
class Label:
  _RE_REMOTE = re.compile('@([\w]*)')
  _RE_PACKAGE = re.compile('//([\w/]*)')
  _RE_TARGET = re.compile('(?::?)([^:]*)')

  def _consumePrefix(self, re, s):
      m = re.match(s)
      if m:
        return m[1], s[len(m[0]):]
      return None, s

  def __init__(self, label):
    label = re.sub(r'/(\w+):(\1)', r'/\1', label)
    self._spec = label
    s = self._spec
    self._remote, s = self._consumePrefix(self._RE_REMOTE, s)
    self._package, s = self._consumePrefix(self._RE_PACKAGE, s)
    self._target, s = self._consumePrefix(self._RE_TARGET, s)

  def __str__(self): return self._spec
  def remote(self): return self._remote
  def package(self): return self._package
  def target(self): return self._target



############################################################
class EvalContext:
  def __init__(self, bazelEnv, thisLabel, debug=lambda x: x):
    self._bazelEnv = bazelEnv
    self._root = self._bazelEnv.root()
    self._label = thisLabel
    self.debug = debug

  def _dummy(self, *args, **kwargs) : pass

  def bazelEnv(self):
    return self._bazelEnv

  def label(self):
    return self._label

  def load(self, bazelPath, *syms, **aliasSyms):
    bzl = Label(bazelPath)
    filePath = self.bazelEnv().resolveFile(bzl)
    if not filePath:
      self.debug(f"load: Ignoring remote load: {bzl}")
      return
    self.debug(f"\_ load({bzl}, {syms}, file={filePath})")
    glo = copy.copy(self.bazelEnv().getGlobals())
    self.debug(f"Before: glo[{len(glo)}]=[{','.join(glo.keys())}]")
    self._execFile(filePath, glo)
    self.debug("Import symbols: [")
    envGlo = self.bazelEnv().getGlobals()
    for sym in syms:
      aliasSyms[sym] = sym
    for alias, sym in aliasSyms.items():
      envGlo[alias] = glo[sym]
    self.debug("]")
    self.debug(f"After: glo[{len(envGlo)}]=[{','.join(envGlo.keys())}]")

  def _execFile(self, file, glo):
    self.debug(f"Compiling {file}")
    with open(os.path.join(self.bazelEnv().root(), file)) as f:
      exec(compile(f.read(), file, 'exec'), glo)
      newline='\n  '
      self.debug(f"exec({file}) completed. Side effects:\nglobals={newline.join(glo)}\n")

  def installIgnoredMembers(self, ignoredMembers: 'list[str]'):
    for memfn in ignoredMembers:
      def annotated(fn, note) :
        def wrapper(*args, **kwargs):
          nonlocal fn
          self.debug(f"# {note} args={args}, kwargs={kwargs}")
          fn(*args, **kwargs)
        return wrapper
      setattr(self, memfn, annotated(self._dummy, memfn))

############################################################
class BuildEvalContext(EvalContext):
  _IGNORED_MEMBERS = [
    'alias',
    'cc_binary',
    'cc_fuzz_test',
    'cc_proto_library',
    'cc_test',
    'config_setting',
    'exports_files',
    'generated_file_staleness_test',
    'genrule',
    'licenses',
    'lua_binary',
    'lua_cclibrary',
    'lua_library',
    'lua_test',
    'make_shell_script',
    'map_dep',
    'package',
    'package_group',
    'proto_library',
    'py_binary',
    'py_library',
    'sh_test',
    'test_suite',
    'upb_amalgamation',
    'upb_proto_library',
    'upb_proto_reflection_library',
  ]

  def __init__(self, env, thisLabel, debug):
    super().__init__(env, thisLabel, debug)
    self.installIgnoredMembers(self._IGNORED_MEMBERS)
    self.native = self  # not sure what this really does

  def _strEval(self, e):
    return f'"{e}"'

  def cc_library(self, **kwargs):
    name = kwargs['name']
    label = Label(f"//{self._label.package()}:{name}")
    self.bazelEnv().addCcLibrary(label, **kwargs)

  def _truth(self, target):
    truths = {
      '//tcmalloc:llvm': self.bazelEnv().toolchainIs('clang'),
      '//conditions:default': True,
    }
    return truths[target]

  def select(self, argDict):
    for k,v in argDict.items():
      self.debug(f"  select: evaluating {k} => {v}")
      if self._truth(k):
        self.debug(f"select True {k}: returning {v}")
        return v
    raise RuntimeError("no condition matched in select map")

  def glob(self, *args): return []


############################################################
class WorkspaceEvalContext(EvalContext):
  _IGNORED_MEMBERS = [
    'http_archive',
    'git_repository',
    'protobuf_deps',
    'rules_proto_dependencies',
    'rules_proto_toolchains',
    'rules_fuzzing_dependencies',
    'rules_fuzzing_init',
    'workspace',
  ]

  def __init__(self, bazelEnv, thisLabel, debug):
    super().__init__(bazelEnv, thisLabel, debug)
    self.installIgnoredMembers(self._IGNORED_MEMBERS)


############################################################
class BazelEnv:
  def __init__(self, sconsEnv, root, debug=lambda x: x):
    self._sconsEnv = sconsEnv
    self._root = root
    self._globals = {}
    self._locals = {}
    self._allTargets = {}
    self._libraries = {}
    self.debug = debug

  def run(self):
    workspace = os.path.join(self._root, 'WORKSPACE')
    self._evalWorkspaceFile(workspace)
    for build in self._findBuildFiles():
      self._evalBuildFile(build)

  def pruneTestOnlyLibraries(self):
    lib = self._libraries
    self._libraries = {}
    for k in lib:
      if 'testonly' not in lib[k]:
        self._libraries[k] = lib[k]

  def libraries(self):
    return self._libraries

  def sconsEnv(self):
    return self._sconsEnv

  def resolveFile(self, label):
    if label.remote():
      return None
    parts = [self._root]
    pkg = label.package()
    if pkg:
      parts.append(pkg)
    tgt = label.target()
    if tgt:
      parts.append(tgt)
    return os.path.join(*parts)

  def root(self):
    return self._root

  def getGlobals(self):
    return self._globals

  def getLocals(self):
    return self._locals

  def toolchainIs(self, category):
    return self.sconsEnv().ToolchainIs(category)

  # Called by the EvalContext to handle cc_library.
  def addCcLibrary(self, label: Label, **kwargs):
    self.debug(f"\_ cc_library('{label}', {kwargs})")
    if 'deps' in kwargs:
      deps = kwargs['deps']
      fqd = set(self._fullyQualifiedDep(str(label), dep) for dep in deps)
      kwargs['deps'] = fqd
    self._libraries[str(label)] = kwargs

  def _getDict(self, obj):
    return {(k, getattr(obj,k)) for k in filter(lambda s : not s.startswith("_"), dir(obj))}

  def _evalWorkspaceFile(self, path):
    self.debug(f"Evaluating WORKSPACE file={path}")
    label = self._labelForFile(path)
    ctx = WorkspaceEvalContext(self, label, self.debug)
    glo = self._globals
    glo.update(self._getDict(ctx))
    ctx._execFile(path, glo)

  def _evalBuildFile(self, path):
    label = self._labelForFile(path)
    self.debug(f"Evaluating BUILD file={path} as label={label}")
    ctx = BuildEvalContext(self, label, self.debug)
    glo = self._globals
    glo.update(self._getDict(ctx))
    ctx._execFile(path, glo)

  def _findBuildFiles(self):
    return glob.iglob(os.path.join(self._root, '**', 'BUILD'), recursive=True)

  def _labelForFile(self, path):
    pkg = path[len(self._root)+1:]  # extract common base with path + '/'
    dirPart = os.path.dirname(pkg)
    basePart = os.path.basename(pkg)
    return Label(f"//{dirPart}:{basePart}")

  def _fullyQualifiedDep(self, libName:str, dep:str) -> str:
    """ dep might need to be relative to lib, make it an absolute label """
    libLabel = Label(libName)
    depLabel = Label(dep)
    if depLabel.package() is None:
      depLabel = Label(f'//{libLabel.package()}:{depLabel.target()}')
    return str(depLabel)

  def resolveDeps(self, exports):
    libs = self.libraries()
    resolved = {}
    todo = [(k, {}) for k in exports]
    while todo:
      top, attrs = todo.pop()

      if top in resolved:
        # merge the 'from' property
        resolved[top].setdefault('from',[]).extend(attrs.get('from',[]))
        continue

      if top not in libs:
        self.debug(f"  unknown dep '{top}'")
        newAttr = copy.copy(attrs)
        newAttr['unknown'] = True
        resolved[top] = newAttr
        continue

      deps = libs[top].get('deps', set())
      if len(deps):
        fqd = set(self._fullyQualifiedDep(top, dep) for dep in deps)
        libs[top]['deps'] = fqd
        self.debug(f' {top}: pushing deps={json.dumps(sorted(list(fqd)), indent=4)}')
        todo.extend((dep, {'from':[top]}) for dep in fqd)

      if top not in resolved:
        resolved[top] = attrs

    return resolved

  def _eliminateSourcelessLib(self, remove):
    bzl = self.libraries()
    for libName, libDef in bzl.items():
      if 'deps' not in libDef:
        continue
      deps = libDef['deps']
      if remove not in deps:
        continue
      deps.remove(remove)
      for innerDep in bzl[remove].get('deps', set()):
        deps.add(innerDep)
    bzl.pop(remove)

  def eliminateSourcelessDeps(self):
    self.debug('eliminate sourceless deps')
    bzl = self.libraries()
    while True:
      class DepsChanged(BaseException): pass
      try:
        for libName,libDef in bzl.items():
          # for each sourceless lib, inline into all dependents and remove it
          if 'srcs' not in libDef:
            self.debug(f'Inline sourceless {libName}')
            self._eliminateSourcelessLib(libName)
            raise DepsChanged()
      except DepsChanged:
        continue
      break

  def eliminateHeadersFromSources(self):
    self.debug('eliminate headers from sources')
    bzl = self.libraries()
    for _,libDef in bzl.items():
      if 'srcs' not in libDef:
        continue
      libDef['srcs'] = [f for f in libDef['srcs'] if not f.endswith('.h')]
