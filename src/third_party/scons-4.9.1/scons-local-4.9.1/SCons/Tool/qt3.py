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

"""Tool-specific initialization for Qt.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.
"""

import os.path
import re

import SCons.Action
import SCons.Builder
import SCons.Defaults
import SCons.Scanner
import SCons.Tool
import SCons.Util
import SCons.Tool.cxx
import SCons.Warnings
cplusplus = SCons.Tool.cxx

class ToolQtWarning(SCons.Warnings.SConsWarning):
    pass

class GeneratedMocFileNotIncluded(ToolQtWarning):
    pass

class QtdirNotFound(ToolQtWarning):
    pass

SCons.Warnings.enableWarningClass(ToolQtWarning)

header_extensions = [".h", ".hxx", ".hpp", ".hh"]
if SCons.Util.case_sensitive_suffixes('.h', '.H'):
    header_extensions.append('.H')

cxx_suffixes = cplusplus.CXXSuffixes


def find_platform_specific_qt3_paths():
    """
    find non-standard QT paths

    If the platform does not put QT tools in standard search paths,
    the path is expected to be set using QT3DIR. SCons violates
    the normal rule of not pulling from the user's environment
    in this case.  However, some test cases try to validate what
    happens when QT3DIR is unset, so we need to try to make a guess.

    :return: a guess at a path
    """

    # qt3_bin_dirs = []
    qt3_bin_dir = None
    if os.path.isfile('/etc/redhat-release'):
        with open('/etc/redhat-release','r') as rr:
            lines = rr.readlines()
            distro = lines[0].split()[0]
        if distro == 'CentOS':
            # Centos installs QT under /usr/{lib,lib64}/qt{4,5,-3.3}/bin
            # so we need to handle this differently
            # qt3_bin_dirs = glob.glob('/usr/lib64/qt*/bin')
            # TODO: all current Fedoras do the same, need to look deeper here.
            qt3_bin_dir = '/usr/lib64/qt-3.3/bin'

    return qt3_bin_dir


QT3_BIN_DIR = find_platform_specific_qt3_paths()

def checkMocIncluded(target, source, env) -> None:
    moc = target[0]
    cpp = source[0]
    # looks like cpp.includes is cleared before the build stage :-(
    # not really sure about the path transformations (moc.cwd? cpp.cwd?) :-/
    path = SCons.Defaults.CScan.path(env, moc.cwd)
    includes = SCons.Defaults.CScan(cpp, env, path)
    if moc not in includes:
        SCons.Warnings.warn(
            GeneratedMocFileNotIncluded,
            "Generated moc file '%s' is not included by '%s'" %
            (str(moc), str(cpp)))

def find_file(filename, paths, node_factory):
    for dir in paths:
        node = node_factory(filename, dir)
        if node.rexists():
            return node
    return None

class _Automoc:
    """
    Callable class, which works as an emitter for Programs, SharedLibraries and
    StaticLibraries.
    """

    def __init__(self, objBuilderName) -> None:
        self.objBuilderName = objBuilderName

    def __call__(self, target, source, env):
        """
        Smart autoscan function. Gets the list of objects for the Program
        or Lib. Adds objects and builders for the special qt3 files.
        """
        try:
            if int(env.subst('$QT3_AUTOSCAN')) == 0:
                return target, source
        except ValueError:
            pass
        try:
            debug = int(env.subst('$QT3_DEBUG'))
        except ValueError:
            debug = 0

        # some shortcuts used in the scanner
        splitext = SCons.Util.splitext
        objBuilder = getattr(env, self.objBuilderName)

        # some regular expressions:
        # Q_OBJECT detection
        q_object_search = re.compile(r'[^A-Za-z0-9]Q_OBJECT[^A-Za-z0-9]')
        # cxx and c comment 'eater'
        #comment = re.compile(r'(//.*)|(/\*(([^*])|(\*[^/]))*\*/)')
        # CW: something must be wrong with the regexp. See also bug #998222
        #     CURRENTLY THERE IS NO TEST CASE FOR THAT

        # The following is kind of hacky to get builders working properly (FIXME)
        objBuilderEnv = objBuilder.env
        objBuilder.env = env
        mocBuilderEnv = env.Moc.env
        env.Moc.env = env

        # make a deep copy for the result; MocH objects will be appended
        out_sources = source[:]

        for obj in source:
            if not obj.has_builder():
                # binary obj file provided
                if debug:
                    print("scons: qt3: '%s' seems to be a binary. Discarded." % str(obj))
                continue
            cpp = obj.sources[0]
            if not splitext(str(cpp))[1] in cxx_suffixes:
                if debug:
                    print("scons: qt3: '%s' is no cxx file. Discarded." % str(cpp))
                # c or fortran source
                continue
            #cpp_contents = comment.sub('', cpp.get_text_contents())
            if debug:
                print("scons: qt3: Getting contents of %s" % cpp)
            cpp_contents = cpp.get_text_contents()
            h=None
            for h_ext in header_extensions:
                # try to find the header file in the corresponding source
                # directory
                hname = splitext(cpp.name)[0] + h_ext
                h = find_file(hname, (cpp.get_dir(),), env.File)
                if h:
                    if debug:
                        print("scons: qt3: Scanning '%s' (header of '%s')" % (str(h), str(cpp)))
                    #h_contents = comment.sub('', h.get_text_contents())
                    h_contents = h.get_text_contents()
                    break
            if not h and debug:
                print("scons: qt3: no header for '%s'." % (str(cpp)))
            if h and q_object_search.search(h_contents):
                # h file with the Q_OBJECT macro found -> add moc_cpp
                moc_cpp = env.Moc(h)
                moc_o = objBuilder(moc_cpp)
                out_sources.append(moc_o)
                #moc_cpp.target_scanner = SCons.Defaults.CScan
                if debug:
                    print("scons: qt3: found Q_OBJECT macro in '%s', moc'ing to '%s'" % (str(h), str(moc_cpp)))
            if cpp and q_object_search.search(cpp_contents):
                # cpp file with Q_OBJECT macro found -> add moc
                # (to be included in cpp)
                moc = env.Moc(cpp)
                env.Ignore(moc, moc)
                if debug:
                    print("scons: qt3: found Q_OBJECT macro in '%s', moc'ing to '%s'" % (str(cpp), str(moc)))
                #moc.source_scanner = SCons.Defaults.CScan
        # restore the original env attributes (FIXME)
        objBuilder.env = objBuilderEnv
        env.Moc.env = mocBuilderEnv

        return (target, out_sources)

AutomocShared = _Automoc('SharedObject')
AutomocStatic = _Automoc('StaticObject')

def _detect_qt3(env):
    """Not really safe, but fast method to detect the QT library"""

    QT3DIR = env.get('QT3DIR',None)
    if not QT3DIR:
        QT3DIR = os.environ.get('QTDIR',None)
    if not QT3DIR:
        moc = env.WhereIs('moc') or env.WhereIs('moc',QT3_BIN_DIR)
        if moc:
            QT3DIR = os.path.dirname(os.path.dirname(moc))
            SCons.Warnings.warn(
                QtdirNotFound,
                "Could not detect qt3, using moc executable as a hint (QT3DIR=%s)" % QT3DIR)
        else:
            QT3DIR = None
            SCons.Warnings.warn(
                QtdirNotFound,
                "Could not detect qt3, using empty QT3DIR")
    return QT3DIR

def uicEmitter(target, source, env):
    adjustixes = SCons.Util.adjustixes
    bs = SCons.Util.splitext(str(source[0].name))[0]
    bs = os.path.join(str(target[0].get_dir()),bs)
    # first target (header) is automatically added by builder
    if len(target) < 2:
        # second target is implementation
        target.append(adjustixes(bs,
                                 env.subst('$QT3_UICIMPLPREFIX'),
                                 env.subst('$QT3_UICIMPLSUFFIX')))
    if len(target) < 3:
        # third target is moc file
        target.append(adjustixes(bs,
                                 env.subst('$QT3_MOCHPREFIX'),
                                 env.subst('$QT3_MOCHSUFFIX')))
    return target, source

def uicScannerFunc(node, env, path):
    lookout = []
    lookout.extend(env['CPPPATH'])
    lookout.append(str(node.rfile().dir))
    includes = re.findall("<include.*?>(.*?)</include>", node.get_text_contents())
    result = []
    for incFile in includes:
        dep = env.FindFile(incFile,lookout)
        if dep:
            result.append(dep)
    return result

uicScanner = SCons.Scanner.ScannerBase(uicScannerFunc,
                                name = "UicScanner",
                                node_class = SCons.Node.FS.File,
                                node_factory = SCons.Node.FS.File,
                                recursive = 0)

def generate(env):
    """Add Builders and construction variables for qt3 to an Environment."""
    CLVar = SCons.Util.CLVar
    Action = SCons.Action.Action
    Builder = SCons.Builder.Builder

    qt3path = _detect_qt3(env)
    if qt3path is None:
        return None

    env.SetDefault(QT3DIR  = qt3path,
                   QT3_BINPATH = os.path.join('$QT3DIR', 'bin'),
                   QT3_CPPPATH = os.path.join('$QT3DIR', 'include'),
                   QT3_LIBPATH = os.path.join('$QT3DIR', 'lib'),
                   QT3_MOC = os.path.join('$QT3_BINPATH','moc'),
                   QT3_UIC = os.path.join('$QT3_BINPATH','uic'),
                   QT3_LIB = 'qt', # may be set to qt-mt

                   QT3_AUTOSCAN = 1, # scan for moc'able sources

                   # Some QT specific flags. I don't expect someone wants to
                   # manipulate those ...
                   QT3_UICIMPLFLAGS = CLVar(''),
                   QT3_UICDECLFLAGS = CLVar(''),
                   QT3_MOCFROMHFLAGS = CLVar(''),
                   QT3_MOCFROMCXXFLAGS = CLVar('-i'),

                   # suffixes/prefixes for the headers / sources to generate
                   QT3_UICDECLPREFIX = '',
                   QT3_UICDECLSUFFIX = '.h',
                   QT3_UICIMPLPREFIX = 'uic_',
                   QT3_UICIMPLSUFFIX = '$CXXFILESUFFIX',
                   QT3_MOCHPREFIX = 'moc_',
                   QT3_MOCHSUFFIX = '$CXXFILESUFFIX',
                   QT3_MOCCXXPREFIX = '',
                   QT3_MOCCXXSUFFIX = '.moc',
                   QT3_UISUFFIX = '.ui',

                   # Commands for the qt3 support ...
                   # command to generate header, implementation and moc-file
                   # from a .ui file
                   QT3_UICCOM = [
                    CLVar('$QT3_UIC $QT3_UICDECLFLAGS -o ${TARGETS[0]} $SOURCE'),
                    CLVar('$QT3_UIC $QT3_UICIMPLFLAGS -impl ${TARGETS[0].file} '
                          '-o ${TARGETS[1]} $SOURCE'),
                    CLVar('$QT3_MOC $QT3_MOCFROMHFLAGS -o ${TARGETS[2]} ${TARGETS[0]}')],
                   # command to generate meta object information for a class
                   # declarated in a header
                   QT3_MOCFROMHCOM = (
                          '$QT3_MOC $QT3_MOCFROMHFLAGS -o ${TARGETS[0]} $SOURCE'),
                   # command to generate meta object information for a class
                   # declarated in a cpp file
                   QT3_MOCFROMCXXCOM = [
                    CLVar('$QT3_MOC $QT3_MOCFROMCXXFLAGS -o ${TARGETS[0]} $SOURCE'),
                    Action(checkMocIncluded,None)])

    # ... and the corresponding builders
    uicBld = Builder(action=SCons.Action.Action('$QT3_UICCOM', '$QT3_UICCOMSTR'),
                     emitter=uicEmitter,
                     src_suffix='$QT3_UISUFFIX',
                     suffix='$QT3_UICDECLSUFFIX',
                     prefix='$QT3_UICDECLPREFIX',
                     source_scanner=uicScanner)
    mocBld = Builder(action={}, prefix={}, suffix={})
    for h in header_extensions:
        act = SCons.Action.Action('$QT3_MOCFROMHCOM', '$QT3_MOCFROMHCOMSTR')
        mocBld.add_action(h, act)
        mocBld.prefix[h] = '$QT3_MOCHPREFIX'
        mocBld.suffix[h] = '$QT3_MOCHSUFFIX'
    for cxx in cxx_suffixes:
        act = SCons.Action.Action('$QT3_MOCFROMCXXCOM', '$QT3_MOCFROMCXXCOMSTR')
        mocBld.add_action(cxx, act)
        mocBld.prefix[cxx] = '$QT3_MOCCXXPREFIX'
        mocBld.suffix[cxx] = '$QT3_MOCCXXSUFFIX'

    # register the builders
    env['BUILDERS']['Uic'] = uicBld
    env['BUILDERS']['Moc'] = mocBld
    static_obj, shared_obj = SCons.Tool.createObjBuilders(env)
    static_obj.add_src_builder('Uic')
    shared_obj.add_src_builder('Uic')

    # We use the emitters of Program / StaticLibrary / SharedLibrary
    # to scan for moc'able files
    # We can't refer to the builders directly, we have to fetch them
    # as Environment attributes because that sets them up to be called
    # correctly later by our emitter.
    env.AppendUnique(PROGEMITTER =[AutomocStatic],
                     SHLIBEMITTER=[AutomocShared],
                     LDMODULEEMITTER=[AutomocShared],
                     LIBEMITTER  =[AutomocStatic],
                     # Of course, we need to link against the qt3 libraries
                     CPPPATH=["$QT3_CPPPATH"],
                     LIBPATH=["$QT3_LIBPATH"],
                     LIBS=['$QT3_LIB'])

def exists(env):
    return _detect_qt3(env)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
