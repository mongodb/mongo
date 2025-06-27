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

"""Common routines for processing Java. """

from __future__ import annotations

import os
import re
import glob
from pathlib import Path

import SCons.Util

java_parsing = True

default_java_version = '1.4'

# a switch for which jdk versions to use the Scope state for smarter
# anonymous inner class parsing.
scopeStateVersions = ('1.8',)

# Glob patterns for use in finding where the JDK is.
#
# These are pairs, (*dir_glob, *version_dir_glob) depending on whether
# a JDK version was requested or not.
# For now only used for Windows, which doesn't install JDK in a
# path that would be in env['ENV']['PATH'].  The specific tool will
# add the discovered path to this.  Since Oracle changed the rules,
# there are many possible vendors, we can't guess them all, but take a shot.
java_win32_dir_glob = 'C:/Program Files*/*/*jdk*/bin'

# On windows, since Java 9, there is a dash between 'jdk' and the version
# string that wasn't there before. this glob should catch either way.
java_win32_version_dir_glob = 'C:/Program Files*/*/*jdk*%s*/bin'

# Glob patterns for use in finding where the JDK headers are.
# These are pairs, *dir_glob used in the general case,
# *version_dir_glob if matching only a specific version.
java_macos_include_dir_glob = '/System/Library/Frameworks/JavaVM.framework/Headers/'
java_macos_version_include_dir_glob = '/System/Library/Frameworks/JavaVM.framework/Versions/%s*/Headers/'

java_linux_include_dirs_glob = [
    '/usr/lib/jvm/default-java/include',
    '/usr/lib/jvm/java-*/include',
    '/opt/oracle-jdk-bin-*/include',
    '/opt/openjdk-bin-*/include',
    '/usr/lib/openjdk-*/include',
]
# Need to match path like below (from Centos 7)
# /usr/lib/jvm/java-1.8.0-openjdk-1.8.0.191.b12-0.el7_5.x86_64/include/
java_linux_version_include_dirs_glob = [
    '/usr/lib/jvm/java-*-sun-%s*/include',
    '/usr/lib/jvm/java-%s*-openjdk*/include',
    '/usr/java/jdk%s*/include',
]

if java_parsing:
    # Parse Java files for class names.
    #
    # This is a really cool parser from Charles Crain
    # that finds appropriate class names in Java source.

    # A regular expression that will find, in a java file:
    #     newlines;
    #     double-backslashes;
    #     a single-line comment "//";
    #     single or double quotes preceeded by a backslash;
    #     single quotes, double quotes, open or close braces, semi-colons,
    #         periods, open or close parentheses;
    #     floating-point numbers;
    #     any alphanumeric token (keyword, class name, specifier);
    #     any alphanumeric token surrounded by angle brackets (generics);
    #     the multi-line comment begin and end tokens /* and */;
    #     array declarations "[]".
    #     Lambda function symbols: ->
    _reToken = re.compile(r'(\n|\\\\|//|\\[\'"]|[\'"{\};.()]|' +
                          r'\d*\.\d*|[A-Za-z_][\w$.]*|<[A-Za-z_]\w+>|' +
                          r'/\*|\*/|\[\]|->)')


    class OuterState:
        """The initial state for parsing a Java file for classes,
        interfaces, and anonymous inner classes."""

        def __init__(self, version=default_java_version) -> None:
            if version not in (
                '1.1',
                '1.2',
                '1.3',
                '1.4',
                '1.5',
                '1.6',
                '1.7',
                '1.8',
                '5',
                '6',
                '9.0',
                '10.0',
                '11.0',
                '12.0',
                '13.0',
                '14.0',
                '15.0',
                '16.0',
                '17.0',
                '18.0',
                '19.0',
                '20.0',
                '21.0',
            ):
                msg = "Java version %s not supported" % version
                raise NotImplementedError(msg)

            self.version = version
            self.listClasses = []
            self.listOutputs = []
            self.stackBrackets = []
            self.brackets = 0
            self.nextAnon = 1
            self.localClasses = []
            self.stackAnonClassBrackets = []
            self.anonStacksStack = [[0]]
            self.package = None

        def trace(self) -> None:
            pass

        def __getClassState(self):
            try:
                return self.classState
            except AttributeError:
                ret = ClassState(self)
                self.classState = ret
                return ret

        def __getPackageState(self):
            try:
                return self.packageState
            except AttributeError:
                ret = PackageState(self)
                self.packageState = ret
                return ret

        def __getAnonClassState(self):
            try:
                return self.anonState
            except AttributeError:
                self.outer_state = self
                ret = SkipState(1, AnonClassState(self))
                self.anonState = ret
                return ret

        def __getSkipState(self):
            try:
                return self.skipState
            except AttributeError:
                ret = SkipState(1, self)
                self.skipState = ret
                return ret

        def _getAnonStack(self):
            return self.anonStacksStack[-1]

        def openBracket(self) -> None:
            self.brackets = self.brackets + 1

        def closeBracket(self) -> None:
            self.brackets = self.brackets - 1
            if len(self.stackBrackets) and \
                    self.brackets == self.stackBrackets[-1]:
                self.listOutputs.append('$'.join(self.listClasses))
                self.localClasses.pop()
                self.listClasses.pop()
                self.anonStacksStack.pop()
                self.stackBrackets.pop()
            if len(self.stackAnonClassBrackets) and \
                    self.brackets == self.stackAnonClassBrackets[-1] and \
                    self.version not in scopeStateVersions:
                self._getAnonStack().pop()
                self.stackAnonClassBrackets.pop()

        def parseToken(self, token):
            if token[:2] == '//':
                return IgnoreState('\n', self)
            elif token == '/*':
                return IgnoreState('*/', self)
            elif token == '{':
                self.openBracket()
            elif token == '}':
                self.closeBracket()
            elif token in ['"', "'"]:
                return IgnoreState(token, self)
            elif token == "new":
                # anonymous inner class
                if len(self.listClasses) > 0:
                    return self.__getAnonClassState()
                return self.__getSkipState()  # Skip the class name
            elif token in ['class', 'interface', 'enum']:
                if len(self.listClasses) == 0:
                    self.nextAnon = 1
                self.stackBrackets.append(self.brackets)
                return self.__getClassState()
            elif token == 'package':
                return self.__getPackageState()
            elif token == '.':
                # Skip the attribute, it might be named "class", in which
                # case we don't want to treat the following token as
                # an inner class name...
                return self.__getSkipState()
            return self

        def addAnonClass(self) -> None:
            """Add an anonymous inner class"""
            if self.version in ('1.1', '1.2', '1.3', '1.4'):
                clazz = self.listClasses[0]
                self.listOutputs.append('%s$%d' % (clazz, self.nextAnon))
            # TODO: shouldn't need to repeat versions here and in OuterState
            elif self.version in (
                '1.5',
                '1.6',
                '1.7',
                '1.8',
                '5',
                '6',
                '9.0',
                '10.0',
                '11.0',
                '12.0',
                '13.0',
                '14.0',
                '15.0',
                '16.0',
                '17.0',
                '18.0',
                '19.0',
                '20.0',
                '21.0',
            ):
                self.stackAnonClassBrackets.append(self.brackets)
                className = []
                className.extend(self.listClasses)
                self._getAnonStack()[-1] = self._getAnonStack()[-1] + 1
                for anon in self._getAnonStack():
                    className.append(str(anon))
                self.listOutputs.append('$'.join(className))

            self.nextAnon = self.nextAnon + 1
            self._getAnonStack().append(0)

        def setPackage(self, package) -> None:
            self.package = package


    class ScopeState:
        """
        A state that parses code within a scope normally,
        within the confines of a scope.
        """

        def __init__(self, old_state) -> None:
            self.outer_state = old_state.outer_state
            self.old_state = old_state
            self.brackets = 0

        def __getClassState(self):
            try:
                return self.classState
            except AttributeError:
                ret = ClassState(self)
                self.classState = ret
                return ret

        def __getAnonClassState(self):
            try:
                return self.anonState
            except AttributeError:
                ret = SkipState(1, AnonClassState(self))
                self.anonState = ret
                return ret

        def __getSkipState(self):
            try:
                return self.skipState
            except AttributeError:
                ret = SkipState(1, self)
                self.skipState = ret
                return ret

        def openBracket(self) -> None:
            self.brackets = self.brackets + 1

        def closeBracket(self) -> None:
            self.brackets = self.brackets - 1

        def parseToken(self, token):
            # if self.brackets == 0:
            #     return self.old_state.parseToken(token)
            if token[:2] == '//':
                return IgnoreState('\n', self)
            elif token == '/*':
                return IgnoreState('*/', self)
            elif token == '{':
                self.openBracket()
            elif token == '}':
                self.closeBracket()
                if self.brackets == 0:
                    self.outer_state._getAnonStack().pop()
                    return self.old_state
            elif token in ['"', "'"]:
                return IgnoreState(token, self)
            elif token == "new":
                # anonymous inner class
                return self.__getAnonClassState()
            elif token == '.':
                # Skip the attribute, it might be named "class", in which
                # case we don't want to treat the following token as
                # an inner class name...
                return self.__getSkipState()
            return self


    class AnonClassState:
        """A state that looks for anonymous inner classes."""

        def __init__(self, old_state) -> None:
            # outer_state is always an instance of OuterState
            self.outer_state = old_state.outer_state
            self.old_state = old_state
            self.brace_level = 0

        def parseToken(self, token):
            # This is an anonymous class if and only if the next
            # non-whitespace token is a bracket. Everything between
            # braces should be parsed as normal java code.
            if token[:2] == '//':
                return IgnoreState('\n', self)
            elif token == '/*':
                return IgnoreState('*/', self)
            elif token == '\n':
                return self
            elif token[0] == '<' and token[-1] == '>':
                return self
            elif token == '(':
                self.brace_level = self.brace_level + 1
                return self
            if self.brace_level > 0:
                if token == 'new':
                    # look further for anonymous inner class
                    return SkipState(1, AnonClassState(self))
                elif token in ['"', "'"]:
                    return IgnoreState(token, self)
                elif token == ')':
                    self.brace_level = self.brace_level - 1
                return self
            if token == '{':
                self.outer_state.addAnonClass()
                if self.outer_state.version in scopeStateVersions:
                    return ScopeState(old_state=self.old_state).parseToken(token)
            return self.old_state.parseToken(token)


    class SkipState:
        """A state that will skip a specified number of tokens before
        reverting to the previous state."""

        def __init__(self, tokens_to_skip, old_state) -> None:
            self.tokens_to_skip = tokens_to_skip
            self.old_state = old_state

        def parseToken(self, token):
            self.tokens_to_skip = self.tokens_to_skip - 1
            if self.tokens_to_skip < 1:
                return self.old_state
            return self


    class ClassState:
        """A state we go into when we hit a class or interface keyword."""

        def __init__(self, outer_state) -> None:
            # outer_state is always an instance of OuterState
            self.outer_state = outer_state

        def parseToken(self, token):
            # the next non-whitespace token should be the name of the class
            if token == '\n':
                return self
            # If that's an inner class which is declared in a method, it
            # requires an index prepended to the class-name, e.g.
            # 'Foo$1Inner'
            # https://github.com/SCons/scons/issues/2087
            if self.outer_state.localClasses and \
                    self.outer_state.stackBrackets[-1] > \
                    self.outer_state.stackBrackets[-2] + 1:
                locals = self.outer_state.localClasses[-1]
                try:
                    idx = locals[token]
                    locals[token] = locals[token] + 1
                except KeyError:
                    locals[token] = 1
                token = str(locals[token]) + token
            self.outer_state.localClasses.append({})
            self.outer_state.listClasses.append(token)
            self.outer_state.anonStacksStack.append([0])
            return self.outer_state


    class IgnoreState:
        """A state that will ignore all tokens until it gets to a
        specified token."""

        def __init__(self, ignore_until, old_state) -> None:
            self.ignore_until = ignore_until
            self.old_state = old_state

        def parseToken(self, token):
            if self.ignore_until == token:
                return self.old_state
            return self


    class PackageState:
        """The state we enter when we encounter the package keyword.
        We assume the next token will be the package name."""

        def __init__(self, outer_state) -> None:
            # outer_state is always an instance of OuterState
            self.outer_state = outer_state

        def parseToken(self, token):
            self.outer_state.setPackage(token)
            return self.outer_state


    def parse_java_file(fn, version=default_java_version):
        with open(fn, "rb") as f:
            data = SCons.Util.to_Text(f.read())
        return parse_java(data, version)


    def parse_java(contents, version=default_java_version, trace=None):
        """Parse a .java file and return a double of package directory,
        plus a list of .class files that compiling that .java file will
        produce"""
        package = None
        initial = OuterState(version)
        currstate = initial
        for token in _reToken.findall(contents):
            # The regex produces a bunch of groups, but only one will
            # have anything in it.
            currstate = currstate.parseToken(token)
            if trace: trace(token, currstate)
        if initial.package:
            package = initial.package.replace('.', os.sep)
        return (package, initial.listOutputs)

else:
    # Don't actually parse Java files for class names.
    #
    # We might make this a configurable option in the future if
    # Java-file parsing takes too long (although it shouldn't relative
    # to how long the Java compiler itself seems to take...).

    def parse_java_file(fn, version=default_java_version):
        """ "Parse" a .java file.

        This actually just splits the file name, so the assumption here
        is that the file name matches the public class name, and that
        the path to the file is the same as the package name.
        """
        return os.path.split(fn)


def get_java_install_dirs(platform, version=None) -> list[str]:
    """ Find possible java jdk installation directories.

    Returns a list for use as `default_paths` when looking up actual
    java binaries with :meth:`SCons.Tool.find_program_path`.
    The paths are sorted by version, latest first.

    Args:
        platform: selector for search algorithm.
        version: if not None, restrict the search to this version.

    Returns:
        list of default paths for jdk.
    """

    if platform == 'win32':
        paths = []
        if version:
            paths = glob.glob(java_win32_version_dir_glob % version)
        else:
            paths = glob.glob(java_win32_dir_glob)

        def win32getvnum(java):
            """ Generates a sort key for win32 jdk versions.

            We'll have gotten a path like ...something/*jdk*/bin because
            that is the pattern we glob for. To generate the sort key,
            extracts the next-to-last component, then trims it further if
            it had a complex name, like 'java-1.8.0-openjdk-1.8.0.312-1',
            to try and put it on a common footing with the more common style,
            which looks like 'jdk-11.0.2'.

            This is certainly fragile, and if someone has a 9.0 it won't
            sort right since this will still be alphabetic, BUT 9.0 was
            not an LTS release and is 30 mos out of support as this note
            is written so just assume it will be okay.
            """
            d = Path(java).parts[-2]
            if not d.startswith('jdk'):
                d = 'jdk' + d.rsplit('jdk', 1)[-1]
            return d

        return sorted(paths, key=win32getvnum, reverse=True)

    # other platforms, do nothing for now: we expect the standard
    # paths to be enough to find a jdk (e.g. use alternatives system)
    return []


def get_java_include_paths(env, javac, version) -> list[str]:
    """Find java include paths for JNI building.

    Cannot be called in isolation - `javac` refers to an already detected
    compiler. Normally would would call :func:`get_java_install_dirs` first
    and then do lookups on the paths it returns before calling us.

    Args:
        env: construction environment, used to extract platform.
        javac: path to detected javac.
        version: if not None, restrict the search to this version.

    Returns:
        list of include directory paths.
    """

    if not javac:
        return []

    # on Windows, we have a path to the actual javac, so look locally
    if env['PLATFORM'] == 'win32':
        javac_bin_dir = os.path.dirname(javac)
        java_inc_dir = os.path.normpath(os.path.join(javac_bin_dir, '..', 'include'))
        paths = [java_inc_dir, os.path.join(java_inc_dir, 'win32')]

    # for the others, we probably found something which isn't in the JDK dir,
    # so use the predefined patterns to glob for an include directory.
    elif env['PLATFORM'] == 'darwin':
        if not version:
            paths = [java_macos_include_dir_glob]
        else:
            paths = sorted(glob.glob(java_macos_version_include_dir_glob % version))
    else:
        base_paths = []
        if not version:
            for p in java_linux_include_dirs_glob:
                base_paths.extend(glob.glob(p))
        else:
            for p in java_linux_version_include_dirs_glob:
                base_paths.extend(glob.glob(p % version))

        paths = []
        for p in base_paths:
            paths.extend([p, os.path.join(p, 'linux')])

    return paths

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
