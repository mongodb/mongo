import argparse
import re

parser = argparse.ArgumentParser()

parser.add_argument("--gdb-test-script")
parser.add_argument("--pip-requirements-script")
parser.add_argument("--pretty-printer-output")
parser.add_argument("--pretty-printer-launcher-infile")
parser.add_argument("--gdb-path")
parser.add_argument("--mongo-toolchain-readelf")
parser.add_argument("--mongo-toolchain-objcopy")
# Because we 'install' these files to a different location then where they are created
# we want to pass the final location of these files rather than where they currently are
parser.add_argument("--final-binary-path")
parser.add_argument("--final-pretty-printer-path")
parser.add_argument("--pretty-printer-launcher-output")

extra_lines = [
    "import os,subprocess,sys,traceback",
    "python_executable = os.environ.get('MONGO_GDB_PYTHON', 'python')",
    "cmd = [python_executable, '-c', 'import os,sys;print(os.linesep.join(sys.path).strip())']",
    "paths = subprocess.check_output(cmd).decode('utf-8').split()",
    "sys.path.extend(paths)",
    "symbols_loaded = False",
    "try:",
    "    symbol_info = gdb.execute('info address main', to_string=True)",
    "    if symbol_info.startswith('Symbol '):",
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
]

args = parser.parse_args()

with open(args.pretty_printer_output, "w") as pretty_printer_output:
    with open(args.pip_requirements_script) as pip_requirements_script:
        pretty_printer_output.write(pip_requirements_script.read())
        pretty_printer_output.write("\n")
    pretty_printer_output.write("\n".join(extra_lines))
    pretty_printer_output.write("\n")
    with open(args.gdb_test_script) as test_script:
        pretty_printer_output.write(test_script.read())

# Verbose and test_args are hardcoded because there are no users of those overrides currently
replacements = {
    "@VERBOSE@": "True",
    "@pretty_printer_test_py@": args.final_pretty_printer_path,
    "@gdb_path@": args.gdb_path,
    "@mongo_toolchain_readelf@": args.mongo_toolchain_readelf,
    "@mongo_toolchain_objcopy@": args.mongo_toolchain_objcopy,
    "@pretty_printer_test_program@": args.final_binary_path,
    "@test_args@": '[""]',
}

escaped_replacements = dict((re.escape(k), v) for k, v in replacements.items())
pattern = re.compile("|".join(escaped_replacements.keys()))

with open(args.pretty_printer_launcher_output, "w") as pretty_printer_launcher_output:
    with open(args.pretty_printer_launcher_infile) as pretty_printer_launcher_infile:
        replaced_text = pattern.sub(
            lambda match: escaped_replacements[re.escape(match.group(0))],
            pretty_printer_launcher_infile.read(),
        )
        pretty_printer_launcher_output.write(replaced_text)
