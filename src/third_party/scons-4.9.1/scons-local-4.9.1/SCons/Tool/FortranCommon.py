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

"""Routines for setting up Fortran, common to all dialects."""

from __future__ import annotations

import re
import os.path

import SCons.Scanner.Fortran
import SCons.Tool
import SCons.Util
from SCons.Action import Action, CommandAction
from SCons.Defaults import StaticObjectEmitter, SharedObjectEmitter


def isfortran(env, source) -> bool:
    """Returns True if *source* has any fortran files in it.

    Only checks based on filename suffixes, does not examine code.
    """
    try:
        fsuffixes = env['FORTRANSUFFIXES']
    except KeyError:
        # If no FORTRANSUFFIXES, no fortran tool, so there is no need to look
        # for fortran sources.
        return False

    if not source:
        # Source might be None for unusual cases like SConf.
        return False
    for s in source:
        if s.sources:
            ext = os.path.splitext(str(s.sources[0]))[1]
            if ext in fsuffixes:
                return True
    return False


def _fortranEmitter(target, source, env) -> Tuple:
    """Common code for Fortran emitter.

    Called by both the static and shared object emitters,
    mainly to account for generated module files.
    """
    node = source[0].rfile()
    if not node.exists() and not node.is_derived():
        print("Could not locate " + str(node.name))
        return [], []

    # This has to match the def_regex in the Fortran scanner
    mod_regex = r"""(?i)^\s*MODULE\s+(?!PROCEDURE|SUBROUTINE|FUNCTION|PURE|ELEMENTAL)(\w+)"""
    cre = re.compile(mod_regex, re.M)
    # Retrieve all USE'd module names
    modules = cre.findall(node.get_text_contents())
    # Remove unique items from the list
    modules = SCons.Util.unique(modules)
    # Convert module name to a .mod filename
    suffix = env.subst('$FORTRANMODSUFFIX', target=target, source=source)
    moddir = env.subst('$FORTRANMODDIR', target=target, source=source)
    modules = [mod.lower() + suffix for mod in modules]
    for module in modules:
        target.append(env.fs.File(module, moddir))
    return target, source


def FortranEmitter(target, source, env) -> Tuple:
    """Create emitter for static objects."""
    target, source = _fortranEmitter(target, source, env)
    return StaticObjectEmitter(target, source, env)


def ShFortranEmitter(target, source, env) -> Tuple:
    """Create emitter for shared objects."""
    target, source = _fortranEmitter(target, source, env)
    return SharedObjectEmitter(target, source, env)


def ComputeFortranSuffixes(suffixes: list[str], ppsuffixes: list[str]) -> None:
    """Update the suffix lists to reflect the platform requirements.

    If upper-cased suffixes can be distinguished from lower, those are
    added to *ppsuffixes*. If not, they are added to *suffixes*.

    Args:
        suffixes: regular Fortran source files
        ppsuffixes: Fortran source files that should be
          be run through the pre-processor
    """
    assert len(suffixes) > 0
    s = suffixes[0]
    sup = s.upper()
    upper_suffixes = [suf.upper() for suf in suffixes]
    if SCons.Util.case_sensitive_suffixes(s, sup):
        ppsuffixes.extend(upper_suffixes)
    else:
        suffixes.extend(upper_suffixes)


def CreateDialectActions(
    dialect: str,
) -> tuple[CommandAction, CommandAction, CommandAction, CommandAction]:
    """Create dialect specific actions."""
    CompAction = Action(f'${dialect}COM ', cmdstr=f'${dialect}COMSTR')
    CompPPAction = Action(f'${dialect}PPCOM ', cmdstr=f'${dialect}PPCOMSTR')
    ShCompAction = Action(f'$SH{dialect}COM ', cmdstr=f'$SH{dialect}COMSTR')
    ShCompPPAction = Action(f'$SH{dialect}PPCOM ', cmdstr=f'$SH{dialect}PPCOMSTR')
    return CompAction, CompPPAction, ShCompAction, ShCompPPAction


def DialectAddToEnv(
    env,
    dialect: str,
    suffixes: list[str],
    ppsuffixes: list[str],
    support_mods: bool = False,
) -> None:
    """Add dialect specific construction variables.

    Args:
        dialect: dialect name
        suffixes: suffixes associated with this dialect
        ppsuffixes: suffixes using cpp associated with this dialect
        support_mods: whether this dialect supports modules
    """
    ComputeFortranSuffixes(suffixes, ppsuffixes)

    fscan = SCons.Scanner.Fortran.FortranScan(f"{dialect}PATH")
    for suffix in suffixes + ppsuffixes:
        SCons.Tool.SourceFileScanner.add_scanner(suffix, fscan)

    env.AppendUnique(FORTRANSUFFIXES=suffixes + ppsuffixes)

    compaction, compppaction, shcompaction, shcompppaction = \
            CreateDialectActions(dialect)
    static_obj, shared_obj = SCons.Tool.createObjBuilders(env)

    for suffix in suffixes:
        static_obj.add_action(suffix, compaction)
        shared_obj.add_action(suffix, shcompaction)
        static_obj.add_emitter(suffix, FortranEmitter)
        shared_obj.add_emitter(suffix, ShFortranEmitter)

    for suffix in ppsuffixes:
        static_obj.add_action(suffix, compppaction)
        shared_obj.add_action(suffix, shcompppaction)
        static_obj.add_emitter(suffix, FortranEmitter)
        shared_obj.add_emitter(suffix, ShFortranEmitter)

    if f'{dialect}FLAGS' not in env:
        env[f'{dialect}FLAGS'] = SCons.Util.CLVar('')
    if f'SH{dialect}FLAGS' not in env:
        env[f'SH{dialect}FLAGS'] = SCons.Util.CLVar(f'${dialect}FLAGS')

    # If a tool does not define fortran prefix/suffix for include path, use C ones
    if f'INC{dialect}PREFIX' not in env:
        env[f'INC{dialect}PREFIX'] = '$INCPREFIX'
    if f'INC{dialect}SUFFIX' not in env:
        env[f'INC{dialect}SUFFIX'] = '$INCSUFFIX'

    env[f'_{dialect}INCFLAGS'] = f'${{_concat(INC{dialect}PREFIX, {dialect}PATH, INC{dialect}SUFFIX, __env__, RDirs, TARGET, SOURCE, affect_signature=False)}}'

    if support_mods:
        env[f'{dialect}COM'] = f'${dialect} -o $TARGET -c $FORTRANCOMMONFLAGS ${dialect}FLAGS $_{dialect}INCFLAGS $_FORTRANMODFLAG $SOURCES'
        env[f'{dialect}PPCOM'] = f'${dialect} -o $TARGET -c $FORTRANCOMMONFLAGS ${dialect}FLAGS $CPPFLAGS $_CPPDEFFLAGS $_{dialect}INCFLAGS $_FORTRANMODFLAG $SOURCES'
        env[f'SH{dialect}COM'] = f'$SH{dialect} -o $TARGET -c $FORTRANCOMMONFLAGS $SH{dialect}FLAGS $_{dialect}INCFLAGS $_FORTRANMODFLAG $SOURCES'
        env[f'SH{dialect}PPCOM'] = f'$SH{dialect} -o $TARGET -c $FORTRANCOMMONFLAGS $SH{dialect}FLAGS $CPPFLAGS $_CPPDEFFLAGS $_{dialect}INCFLAGS $_FORTRANMODFLAG $SOURCES'
    else:
        env[f'{dialect}COM'] = f'${dialect} -o $TARGET -c $FORTRANCOMMONFLAGS ${dialect}FLAGS $_{dialect}INCFLAGS $SOURCES'
        env[f'{dialect}PPCOM'] = f'${dialect} -o $TARGET -c $FORTRANCOMMONFLAGS ${dialect}FLAGS $CPPFLAGS $_CPPDEFFLAGS $_{dialect}INCFLAGS $SOURCES'
        env[f'SH{dialect}COM'] = f'$SH{dialect} -o $TARGET -c $FORTRANCOMMONFLAGS $SH{dialect}FLAGS $_{dialect}INCFLAGS $SOURCES'
        env[f'SH{dialect}PPCOM'] = f'$SH{dialect} -o $TARGET -c $FORTRANCOMMONFLAGS $SH{dialect}FLAGS $CPPFLAGS $_CPPDEFFLAGS $_{dialect}INCFLAGS $SOURCES'


def add_fortran_to_env(env) -> None:
    """Add Builders and construction variables for Fortran/generic."""
    FortranSuffixes = env.get('FORTRANFILESUFFIXES', ['.f', '.for', '.ftn'])
    FortranPPSuffixes = env.get('FORTRANPPFILESUFFIXES', ['.fpp', '.FPP'])
    DialectAddToEnv(env, "FORTRAN", FortranSuffixes, FortranPPSuffixes, support_mods=True)

    # Module support
    env['FORTRANMODPREFIX'] = ''     # like $LIBPREFIX
    env['FORTRANMODSUFFIX'] = '.mod' # like $LIBSUFFIX
    env['FORTRANMODDIR'] = ''          # where the compiler should place .mod files
    env['FORTRANMODDIRPREFIX'] = ''    # some prefix to $FORTRANMODDIR - similar to $INCPREFIX
    env['FORTRANMODDIRSUFFIX'] = ''    # some suffix to $FORTRANMODDIR - similar to $INCSUFFIX
    env['_FORTRANMODFLAG'] = '$( ${_concat(FORTRANMODDIRPREFIX, FORTRANMODDIR, FORTRANMODDIRSUFFIX, __env__, RDirs, TARGET, SOURCE)} $)'

def add_f77_to_env(env) -> None:
    """Add Builders and construction variables for f77 dialect."""
    F77Suffixes = env.get('F77FILESUFFIXES', ['.f77'])
    F77PPSuffixes = env.get('F77PPFILESUFFIXES', [])
    DialectAddToEnv(env, "F77", F77Suffixes, F77PPSuffixes)

def add_f90_to_env(env) -> None:
    """Add Builders and construction variables for f90 dialect."""
    F90Suffixes = env.get('F90FILESUFFIXES', ['.f90'])
    F90PPSuffixes = env.get('F90PPFILESUFFIXES', [])
    DialectAddToEnv(env, "F90", F90Suffixes, F90PPSuffixes, support_mods=True)

def add_f95_to_env(env) -> None:
    """Add Builders and construction variables for f95 dialect."""
    F95Suffixes = env.get('F95FILESUFFIXES', ['.f95'])
    F95PPSuffixes = env.get('F95PPFILESUFFIXES', [])
    DialectAddToEnv(env, "F95", F95Suffixes, F95PPSuffixes, support_mods=True)

def add_f03_to_env(env) -> None:
    """Add Builders and construction variables for f03 dialect."""
    F03Suffixes = env.get('F03FILESUFFIXES', ['.f03'])
    F03PPSuffixes = env.get('F03PPFILESUFFIXES', [])
    DialectAddToEnv(env, "F03", F03Suffixes, F03PPSuffixes, support_mods=True)

def add_f08_to_env(env) -> None:
    """Add Builders and construction variables for f08 dialect."""
    F08Suffixes = env.get('F08FILESUFFIXES', ['.f08'])
    F08PPSuffixes = env.get('F08PPFILESUFFIXES', [])
    DialectAddToEnv(env, "F08", F08Suffixes, F08PPSuffixes, support_mods=True)

def add_all_to_env(env) -> None:
    """Add builders and construction variables for all supported dialects."""
    add_fortran_to_env(env)
    add_f77_to_env(env)
    add_f90_to_env(env)
    add_f95_to_env(env)
    add_f03_to_env(env)
    add_f08_to_env(env)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
