# Copyright 2020 MongoDB Inc.
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
#
"""Pseudo-builders for building and registering tests for pretty printers."""
import subprocess
import os
import sys

import SCons
from SCons.Script import Chmod

not_building_already_warned = False


def print_warning(message: str):
    global not_building_already_warned
    if not not_building_already_warned:
        not_building_already_warned = True
        print(message)


def exists(env):
    return True


ninja_fake_testlist = None


def build_pretty_printer_test(env, target, **kwargs):

    if not isinstance(target, list):
        target = [target]

    if env.GetOption('ninja') != 'disabled':
        return []

    gdb_bin = None
    if env.get('GDB'):
        gdb_bin = env.get('GDB')
    elif env.ToolchainIs('gcc', 'clang'):
        # Always prefer v4 gdb, otherwise try anything in the path
        gdb_bin = env.WhereIs('gdb', ['/opt/mongodbtoolchain/v4/bin']) or env.WhereIs('gdb')

    if gdb_bin is None:
        print_warning("Can't find gdb, not building pretty printer tests.")
        return []

    test_component = {"dist-test", "pretty-printer-tests-pyonly"}

    if "AIB_COMPONENTS_EXTRA" in kwargs:
        kwargs["AIB_COMPONENTS_EXTRA"] = set(kwargs["AIB_COMPONENTS_EXTRA"]).union(test_component)
    else:
        kwargs["AIB_COMPONENTS_EXTRA"] = list(test_component)

    python_bin = sys.executable

    test_program = kwargs.get("TEST_PROGRAM", ['$DESTDIR/$PREFIX/bin/mongod'])
    if isinstance(test_program, list):
        test_program = test_program[0]
    test_args = kwargs.get('TEST_ARGS', [])
    gdb_test_script = env.File(target[0]).srcnode().abspath

    if not gdb_test_script:
        env.FatalError(
            f"{target[0]}: You must supply a gdb python script to use in the pretty printer test.")

    with open(gdb_test_script) as test_script:
        verify_reqs_file = env.File('#site_scons/mongo/pip_requirements.py')

        gen_test_script = env.Textfile(
            target=os.path.basename(gdb_test_script),
            source=verify_reqs_file.get_contents().decode('utf-8').split('\n') + [
                "import os,subprocess,sys,traceback",
                "cmd = 'python -c \"import os,sys;print(os.linesep.join(sys.path).strip())\"'",
                "paths = subprocess.check_output(cmd,shell=True).decode('utf-8').split()",
                "sys.path.extend(paths)",
                "symbols_loaded = False",
                "try:",
                "    if gdb.objfiles()[0].lookup_global_symbol('main') is not None:",
                "        symbols_loaded = True",
                "except Exception:",
                "    pass",
                "if not symbols_loaded:",
                r"    gdb.write('Could not find main symbol, debug info may not be loaded.\n')",
                r"    gdb.write('TEST FAILED -- No Symbols.\\\n')",
                "    gdb.execute('quit 1', to_string=True)",
                "else:",
                r"    gdb.write('Symbols loaded.\n')",
                "gdb.execute('set confirm off')",
                "gdb.execute('source .gdbinit')",
                "try:",
                "    verify_requirements(executable='python3')",
                "except MissingRequirements as ex:",
                "    print(ex)",
                "    print('continuing testing anyways!')",
                "except Exception as exc:",
                "    print('ERROR: failed while verifying requirements.')",
                "    traceback.print_exc()",
                "    sys.exit(1)",
            ] + [line.rstrip() for line in test_script.readlines()])

        gen_test_script_install = env.AutoInstall(
            target='$PREFIX_BINDIR',
            source=gen_test_script,
            AIB_ROLE='runtime',
            AIB_COMPONENT='pretty-printer-tests',
            AIB_COMPONENTS_EXTRA=kwargs["AIB_COMPONENTS_EXTRA"],
        )

    pretty_printer_test_launcher = env.Substfile(
        target=f'pretty_printer_test_launcher_{target[0]}',
        source='#/src/mongo/util/pretty_printer_test_launcher.py.in', SUBST_DICT={
            '@VERBOSE@':
                str(env.Verbose()),
            '@pretty_printer_test_py@':
                gen_test_script_install[0].path,
            '@gdb_path@':
                gdb_bin,
            '@pretty_printer_test_program@':
                env.File(test_program).path,
            '@test_args@':
                '["' + '", "'.join([env.subst(arg, target=target) for arg in test_args]) + '"]',
        }, AIB_ROLE='runtime', AIB_COMPONENT='pretty-printer-tests',
        AIB_COMPONENTS_EXTRA=kwargs["AIB_COMPONENTS_EXTRA"])
    env.Depends(
        pretty_printer_test_launcher[0],
        ([] if env.get('GDB_PPTEST_PYONLY') else [test_program]) + [gen_test_script_install],
    )
    env.AddPostAction(pretty_printer_test_launcher[0],
                      Chmod(pretty_printer_test_launcher[0], 'ugo+x'))

    pretty_printer_test_launcher_install = env.AutoInstall(
        target='$PREFIX_BINDIR',
        source=pretty_printer_test_launcher,
        AIB_ROLE='runtime',
        AIB_COMPONENT='pretty-printer-tests',
        AIB_COMPONENTS_EXTRA=kwargs["AIB_COMPONENTS_EXTRA"],
    )

    def new_scanner(node, env, path=()):
        source_binary = getattr(
            env.File(env.get('TEST_PROGRAM')).attributes, 'AIB_INSTALL_FROM', None)
        if source_binary:
            debug_files = getattr(env.File(source_binary).attributes, 'separate_debug_files', None)
            if debug_files:
                if debug_files:
                    installed_debug_files = getattr(
                        env.File(debug_files[0]).attributes, 'AIB_INSTALLED_FILES', None)
                    if installed_debug_files:
                        if env.Verbose():
                            print(
                                f"Found and installing pretty_printer_test {node} test_program {env.File(env.get('TEST_PROGRAM'))} debug file {installed_debug_files[0]}"
                            )
                        return installed_debug_files
        if env.Verbose():
            print(f"Did not find separate debug files for pretty_printer_test {node}")
        return []

    scanner = SCons.Scanner.Scanner(function=new_scanner)

    run_test = env.Command(target='+' + os.path.splitext(os.path.basename(gdb_test_script))[0],
                           source=pretty_printer_test_launcher_install, action=str(
                               pretty_printer_test_launcher_install[0]), TEST_PROGRAM=test_program,
                           target_scanner=scanner)
    env.Pseudo(run_test)
    env.Alias('+' + os.path.splitext(os.path.basename(gdb_test_script))[0], run_test)
    env.Depends(
        pretty_printer_test_launcher_install,
        ([] if env.get('GDB_PPTEST_PYONLY') else [test_program]) + [gen_test_script_install],
    )

    env.RegisterTest('$PRETTY_PRINTER_TEST_LIST', pretty_printer_test_launcher_install[0])
    env.Alias("$PRETTY_PRINTER_TEST_ALIAS", pretty_printer_test_launcher_install[0])
    env.Alias('+pretty-printer-tests', run_test)
    return run_test


def generate(env):
    global ninja_fake_testlist
    if env.GetOption('ninja') != 'disabled' and ninja_fake_testlist is None:
        print_warning("Can't run pretty printer tests with ninja.")
        ninja_fake_testlist = env.Command(
            '$PRETTY_PRINTER_TEST_LIST', __file__,
            "type nul >>$TARGET" if sys.platform == 'win32' else "touch $TARGET")
    else:
        env.TestList("$PRETTY_PRINTER_TEST_LIST", source=[])

    env.AddMethod(build_pretty_printer_test, "PrettyPrinterTest")
    alias = env.Alias("$PRETTY_PRINTER_TEST_ALIAS", "$PRETTY_PRINTER_TEST_LIST")
    env.Alias('+pretty-printer-tests', alias)
