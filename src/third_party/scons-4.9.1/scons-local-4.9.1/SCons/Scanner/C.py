# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""Dependency scanner for C/C++ code.

Two scanners are defined here: the default CScanner, and the optional
CConditionalScanner, which must be explicitly selected by calling
add_scanner() for each affected suffix.
"""

from typing import Dict

import SCons.Node.FS
import SCons.cpp
import SCons.Util
from . import ClassicCPP, FindPathDirs


class SConsCPPScanner(SCons.cpp.PreProcessor):
    """SCons-specific subclass of the cpp.py module's processing.

    We subclass this so that: 1) we can deal with files represented
    by Nodes, not strings; 2) we can keep track of the files that are
    missing.
    """
    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.missing = []

    def initialize_result(self, fname) -> None:
        self.result = SCons.Util.UniqueList([fname])

    def finalize_result(self, fname):
        return self.result[1:]

    def find_include_file(self, t):
        keyword, quote, fname = t
        result = SCons.Node.FS.find_file(fname, self.searchpath[quote])
        if not result:
            self.missing.append((fname, self.current_file))
        return result

    def read_file(self, file) -> str:
        try:
            return file.rfile().get_text_contents()
        except OSError as e:
            self.missing.append((file, self.current_file))
            return ''

def dictify_CPPDEFINES(env, replace: bool = False) -> dict:
    """Return CPPDEFINES converted to a dict for preprocessor emulation.

    The concept is similar to :func:`~SCons.Defaults.processDefines`:
    turn the values stored in an internal form in ``env['CPPDEFINES']``
    into one needed for a specific context - in this case the cpp-like
    work the C/C++ scanner will do. We can't reuse ``processDefines``
    output as that's a list of strings for the command line. We also can't
    pass the ``CPPDEFINES`` variable directly to the ``dict`` constructor,
    as SCons allows it to be stored in several different ways - it's only
    after ``Append`` and relatives has been called we know for sure it will
    be a deque of tuples.

    If requested (*replace* is true), simulate some of the macro
    replacement that would take place if an actual preprocessor ran,
    to avoid some conditional inclusions comeing out wrong.  A bit
    of an edge case, but does happen (GH #4623). See 6.10.5 in the C
    standard and 15.6 in the C++ standard).

    Args:
        replace: if true, simulate macro replacement

    .. versionchanged:: 4.9.0
       Simple macro replacement added, and *replace* arg to enable it.
    """
    def _replace(mapping: Dict) -> Dict:
        """Simplistic macro replacer for dictify_CPPDEFINES.

        Scan *mapping* for a value that is the same as a key in the dict,
        and replace with the value of that key; the process is repeated a few
        times, but not forever in case someone left a case that can't be
        fully resolved.  This is a cheap approximation of the preprocessor's
        macro replacement rules with no smarts - it doesn't "look inside"
        the values, so only triggers on object-like macros, not on
        function-like macros, and will not work on complex values, e.g.
        a value like ``(1UL << PR_MTE_TCF_SHIFT)`` would not have
        ``PR_MTE_TCF_SHIFT`` replaced if that was also a key in ``CPPDEFINES``.

        Args:
            mapping: a dictionary representing macro names and replacements.

        Returns:
            a dictionary with replacements made.
        """
        old_ns = mapping
        loops = 0
        while loops < 5:  # don't recurse forever in case there's circular data
            # this was originally written as a dict comprehension, but unrolling
            # lets us add a finer-grained check for whether another loop is
            # needed, rather than comparing two dicts to see if one changed.
            again = False
            ns = {}
            for k, v in old_ns.items():
                if v in old_ns:
                    ns[k] = old_ns[v]
                    if not again and ns[k] != v:
                        again = True
                else:
                    ns[k] = v
            if not again:
                break
            old_ns = ns
            loops += 1
        return ns

    cppdefines = env.get('CPPDEFINES', {})
    if not cppdefines:
        return {}

    if SCons.Util.is_Tuple(cppdefines):
        # single macro defined in a tuple
        try:
            return {cppdefines[0]: cppdefines[1]}
        except IndexError:
            return {cppdefines[0]: None}

    if SCons.Util.is_Sequence(cppdefines):
        # multiple (presumably) macro defines in a deque, list, etc.
        result = {}
        for c in cppdefines:
            if SCons.Util.is_Sequence(c):
                try:
                    result[c[0]] = c[1]
                except IndexError:
                    # could be a one-item sequence
                    result[c[0]] = None
            elif SCons.Util.is_String(c):
                try:
                    name, value = c.split('=')
                    result[name] = value
                except ValueError:
                    result[c] = None
            else:
                # don't really know what to do here
                result[c] = None
        if replace:
            return _replace(result)
        return(result)

    if SCons.Util.is_String(cppdefines):
        # single macro define in a string
        try:
            name, value = cppdefines.split('=')
            return {name: value}
        except ValueError:
            return {cppdefines: None}

    if SCons.Util.is_Dict(cppdefines):
        # already in the desired form
        if replace:
            return _replace(cppdefines)
        return cppdefines

    return {cppdefines: None}

class SConsCPPScannerWrapper:
    """The SCons wrapper around a cpp.py scanner.

    This is the actual glue between the calling conventions of generic
    SCons scanners, and the (subclass of) cpp.py class that knows how
    to look for #include lines with reasonably real C-preprocessor-like
    evaluation of #if/#ifdef/#else/#elif lines.
    """

    def __init__(self, name, variable) -> None:
        self.name = name
        self.path = FindPathDirs(variable)

    def __call__(self, node, env, path=()):
        cpp = SConsCPPScanner(
            current=node.get_dir(),
            cpppath=path,
            dict=dictify_CPPDEFINES(env, replace=True),
        )
        result = cpp(node)
        for included, includer in cpp.missing:
            SCons.Warnings.warn(
                SCons.Warnings.DependencyWarning,
                "No dependency generated for file: %s (included from: %s) "
                "-- file not found" % (included, includer),
            )
        return result

    def recurse_nodes(self, nodes):
        return nodes

    def select(self, node):
        return self

def CScanner():
    """Return a prototype Scanner instance for scanning source files
    that use the C pre-processor"""

    # Here's how we would (or might) use the CPP scanner code above that
    # knows how to evaluate #if/#ifdef/#else/#elif lines when searching
    # for #includes.  This is commented out for now until we add the
    # right configurability to let users pick between the scanners.
    # return SConsCPPScannerWrapper("CScanner", "CPPPATH")

    cs = ClassicCPP(
        "CScanner",
        "$CPPSUFFIXES",
        "CPPPATH",
        r'^[ \t]*#[ \t]*(?:include|import)[ \t]*(<|")([^>"]+)(>|")',
    )
    return cs


#
# ConditionalScanner
#


class SConsCPPConditionalScanner(SCons.cpp.PreProcessor):
    """SCons-specific subclass of the cpp.py module's processing.

    We subclass this so that: 1) we can deal with files represented
    by Nodes, not strings; 2) we can keep track of the files that are
    missing.
    """

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self.missing = []
        self._known_paths = []

    def initialize_result(self, fname) -> None:
        self.result = SCons.Util.UniqueList([fname])

    def find_include_file(self, t):
        keyword, quote, fname = t
        paths = tuple(self._known_paths) + self.searchpath[quote]
        if quote == '"':
            paths = (self.current_file.dir,) + paths
        result = SCons.Node.FS.find_file(fname, paths)
        if result:
            result_path = result.get_abspath()
            for p in self.searchpath[quote]:
                if result_path.startswith(p.get_abspath()):
                    self._known_paths.append(p)
                    break
        else:
            self.missing.append((fname, self.current_file))
        return result

    def read_file(self, file) -> str:
        try:
            return file.rfile().get_text_contents()
        except OSError:
            self.missing.append((file, self.current_file))
            return ""


class SConsCPPConditionalScannerWrapper:
    """
    The SCons wrapper around a cpp.py scanner.

    This is the actual glue between the calling conventions of generic
    SCons scanners, and the (subclass of) cpp.py class that knows how
    to look for #include lines with reasonably real C-preprocessor-like
    evaluation of #if/#ifdef/#else/#elif lines.
    """

    def __init__(self, name, variable) -> None:
        self.name = name
        self.path = FindPathDirs(variable)

    def __call__(self, node, env, path=(), depth=-1):
        cpp = SConsCPPConditionalScanner(
            current=node.get_dir(),
            cpppath=path,
            dict=dictify_CPPDEFINES(env),
            depth=depth,
        )
        result = cpp(node)
        for included, includer in cpp.missing:
            fmt = "No dependency generated for file: %s (included from: %s) -- file not found"
            SCons.Warnings.warn(
                SCons.Warnings.DependencyWarning, fmt % (included, includer)
            )
        return result

    def recurse_nodes(self, nodes):
        return nodes

    def select(self, node):
        return self


def CConditionalScanner():
    """
    Return an advanced conditional Scanner instance for scanning source files

    Interprets C/C++ Preprocessor conditional syntax
    (#ifdef, #if, defined, #else, #elif, etc.).
    """
    return SConsCPPConditionalScannerWrapper("CConditionalScanner", "CPPPATH")


# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
