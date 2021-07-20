#! /usr/bin/env python3

"""
This program provides an CLI interface to check and generate Evergreen configuration.
"""

import os
import sys
import re
import subprocess

try:
    import docopt
except Exception as e:
    modules = "docopt"
    print("ERROR [%s]: %s" % (sys.argv[0], e))
    print("Use pip to install the required library:")
    print("  pip install %s" % modules)
    sys.exit(0)

TEST_TYPES = ('make_check', 'csuite')
EVG_CFG_FILE = "test/evergreen.yml"
CSUITE_TEST_DIR = "test/csuite"
MAKE_CHECK_TEST_TMPLT = "test/evergreen/make_check_test_evg_task.template"
CSUITE_TEST_TMPLT = "test/evergreen/csuite_test_evg_task.template"
MAKE_CHECK_TEST_SEARCH_STR = "  # End of normal make check test tasks"
CSUITE_TEST_SEARCH_STR = "  # End of csuite test tasks"

# This list of sub directories will be skipped from checking.
# They are not expected to trigger any 'make check' testing.
make_check_subdir_skips = [
    "test/csuite",  # csuite has its own set of Evergreen tasks, skip the checking here
    "test/cppsuite"
]

prog=sys.argv[0]
PROGNAME = os.path.basename(prog)
DESCRIPTION = 'Evergreen configuration helper 0.1'
USAGE = """
Evergreen configuration helper.

Usage:
  {progname} check [-t <test_type>] [-v]
  {progname} generate [-t <test_type>] [-v]
  {progname} (-h | --help)

Options:
  -h --help     Show this screen.
  -t TEST_TYPE  The type of test to be checked/generated.
  -v            Enable verbose logging.
  check         Check if any missing tests that should be added into Evergreen configuration.
  generate      Generate Evergreen configuration for missing tests.
""".format(progname=PROGNAME)

verbose = False

def debug(msg):
    """ A wrapper to print function with checking of verbose flag """

    if verbose is True:
        print(msg)

def run(cmd):
    """ Run a shell command and return the output """

    if isinstance(cmd, str):
        cmd = cmd.split()

    try:
        output = subprocess.check_output(cmd, stderr=subprocess.STDOUT).strip().decode()
    except Exception as e:
        sys.exit("ERROR [%s]: %s" % (prog, e))

    return output

def find_tests_missing_evg_cfg(test_type, dirs, evg_cfg_file):
    """
    Check the list of 'make check' directories to find out those
    that are missing from the Evergreen configuration file.

    The existing Evergreen configuration is expected to have
    included one task for each applicable 'make check' directory.
    Newly added 'make check' directories that involve real tests
    should be identified by this function.
    """

    if not dirs:
        sys.exit("\nNo %s directory is found ..." % test_type)

    assert os.path.isfile(evg_cfg_file), "'%s' does not exist" % evg_cfg_file

    with open(evg_cfg_file, 'r') as f:
        evg_cfg = f.readlines()

    debug('\n')
    missing_tests = {}
    for d in dirs:
        # Figure out the Evergreen task name from the directory name

        if test_type == 'make_check':
            # The Evergreen task name for each 'make check' test is worked out from directory name
            # E.g. for 'make check' directory 'test/cursor_order', the corresponding Evergreen
            # task name will be 'cursor-order-test'.

            dir_wo_test_prefix = d[len("test/"):] if d.startswith("test/") else d
            evg_task_name = dir_wo_test_prefix.replace('/', '-').replace('_', '-') + '-test'
            debug("Evergreen task name for make check directory '%s' is: %s" % (d, evg_task_name))

        elif test_type == 'csuite':
            # The Evergreen task name for each 'csuite' test is worked out from sub directory name
            # E.g. for 'test/csuite' sub directory 'wt3184_dup_index_collator', the corresponding
            # Evergreen task name will be 'csuite-wt3184-dup-index-collator-test'.

            evg_task_name = 'csuite-' + d.replace('_', '-') + '-test'
            debug("Evergreen task name for csuite sub directory '%s' is: %s" % (d, evg_task_name))

        else:
            sys.exit("Unsupported test_type '%s'" % test_type)

        # Check if the Evergreen task name exists in current Evergreen configuration
        if evg_task_name in str(evg_cfg):
            # Match found
            continue
        else:
            # Missing task/test found
            missing_tests.update({evg_task_name: d})
            print("Task '%s' (for directory '%s') is missing in %s!" %
                  (evg_task_name, d, evg_cfg_file))

    return missing_tests

def get_make_check_dirs():
    """
    Figure out the 'make check' directories that are applicable for testing
    Directories with Makefile.am containing 'TESTS =' are the ones require test.
    Skip a few known directories that do not require test or covered separately.
    """

    # Make sure we are under the repo top level directory
    os.chdir(run('git rev-parse --show-toplevel'))

    # Search keyword in Makefile.am to identify directories that involve test configuration.
    # Need to use subprocess 'shell=True' to get the expected shell command output.
    cmd = "find . -name Makefile.am -exec grep -H -e '^TESTS =' {} \; | cut -d: -f1 | cut -c3-"
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, shell=True)
    mkfiles_with_tests = p.stdout.readlines()

    # Need some string manipulation work here against the subprocess output.
    # Cast elements to string, and strip the ending from the string to get directory names.
    ending = '/Makefile.am\n'
    dirs_with_tests = [d.decode('utf-8')[:-len(ending)] for d in mkfiles_with_tests]
    debug("dirs_with_tests: %s" % dirs_with_tests)

    # Remove directories in the skip list
    make_check_dirs = [d for d in dirs_with_tests if d not in make_check_subdir_skips]
    debug("\nThe list of 'make check' dirs that should be included in Evergreen configuration:\n %s"
          % make_check_dirs)

    return make_check_dirs

def get_csuite_dirs():
    """
    Figure out the 'make check' directories that are applicable for testing
    Loop through the list of sub directories under test/csuite/ and skip those WT_TEST.* directories
    """

    assert os.path.isdir(CSUITE_TEST_DIR), "'%s' does not exist" % CSUITE_TEST_DIR

    # Retrieve all sub directories under 'test/csuite' directory
    subdirs = [x[1] for x in os.walk(CSUITE_TEST_DIR)][0]

    # Remove directories with name starting with 'WT_TEST' or '.'
    regex = re.compile(r'^(WT_TEST|\.)')
    csuite_dirs = [d for d in subdirs if not regex.search(d)]
    debug("\nThe list of 'csuite' dirs that should be included in Evergreen configuration:\n %s"
          % csuite_dirs)

    return csuite_dirs

def check_missing_tests(test_type):
    """
    Check to see if any tests are missing from the Evergreen configuration.
    Loop through the list of directories in 'Make.subdirs' file and skip a few known
    directories that do not require any test.
    """

    # Retrieve the directories that are applicable for testing based on test type
    if test_type == 'make_check':
        test_dirs = get_make_check_dirs()
    elif test_type == 'csuite':
        test_dirs = get_csuite_dirs()
    else:
        sys.exit("Unsupported test type '%s'" % test_type)

    return find_tests_missing_evg_cfg(test_type, test_dirs, EVG_CFG_FILE)

def get_evg_task_template(test_type):
    """ Retrieve the Evergreen task template based on test type """

    if test_type == 'make_check':
        template_file = MAKE_CHECK_TEST_TMPLT
    elif test_type == 'csuite':
        template_file = CSUITE_TEST_TMPLT
    else:
        sys.exit("Unsupported test type '%s'" % test_type)

    assert os.path.isfile(template_file), "'%s' does not exist" % template_file

    with open(template_file, 'r') as f:
        template = f.read()

    return template

def get_search_string(test_type):
    """ Retrieve the search string based on test_type """

    if test_type == 'make_check':
        search_str = MAKE_CHECK_TEST_SEARCH_STR
    elif test_type == 'csuite':
        search_str = CSUITE_TEST_SEARCH_STR
    else:
        sys.exit("Unsupported test type '%s'" % test_type)

    return search_str

def generate_evg_cfg_for_missing_tests(test_type, missing_tests):
    """
    Generate the Evergreen configuration for the missing tests based on test type.
    Will apply the newly generated changes to the Evergreen configuration file directly.
    """

    if not missing_tests:
        sys.exit("No missing test is found, exiting ...")
    debug("missing_tests: %s" % missing_tests)

    template = get_evg_task_template(test_type)

    evg_cfg_to_add = ''
    for task, dir in missing_tests.items():
        # Replace variables in the template with valid values for each missing test
        evg_cfg_to_add += template.replace('{{task_name}}', task).replace('{{test_dir}}', dir)

    print("\nBelow Evergreen configuration snippet will be added into existing %s file: \n\n%s"
          % (EVG_CFG_FILE, evg_cfg_to_add))

    assert os.path.isfile(EVG_CFG_FILE), "'%s' does not exist" % EVG_CFG_FILE

    search_str = get_search_string(test_type)
    debug("search_str: '%s'" % search_str)

    with open(EVG_CFG_FILE, 'r') as f:
        evg_cfg = f.read()
        # Insert the new Evergreen configuration for missing tests.
        # Use the search string to help locating the position for insert.
        new_evg_cfg = evg_cfg.replace(search_str, evg_cfg_to_add + search_str)

    # Write the changes back to the file
    with open(EVG_CFG_FILE, 'w') as f:
        f.write(new_evg_cfg)

def evg_cfg(action, test_type):
    """
    The main entry function that calls different functions based on action type and test type
    """

    # Make sure the program is run under a checkout of wiredtiger repository
    # We could get different string outputs when running 'git config remote.origin.url':
    #   - 'git@github.com:wiredtiger/wiredtiger.git' (if run locally)
    #   - 'ssh://git@github.com/wiredtiger/wiredtiger.git' (if run through SSH)
    #   - 'git://github.com/wiredtiger/wiredtiger' (if cloned anonymously)
    output = run('git config remote.origin.url')
    if not 'github.com' in output or (not 'wiredtiger.git' in output and output != 'git://github.com/wiredtiger/wiredtiger'):
        sys.exit("ERROR [%s]: need to run this script inside a wiredtiger repo" % prog)

    # Change directory to repo top level
    os.chdir(run('git rev-parse --show-toplevel'))

    missing_tests = {}
    if action == 'check':
        if test_type in TEST_TYPES:
            missing_tests = check_missing_tests(test_type)
        elif test_type == 'all':
            # Check each of the test type
            for t in TEST_TYPES:
                # Aggregate the missing tests for each test type together here
                missing_tests.update(check_missing_tests(t))
        else:
            sys.exit("Unsupported test type '%s'" % test_type)

        # If any missing test is identified, prompt users to run the 'generate' action
        # which will auto-generate the Evergreen configuration for those missing tests.
        if missing_tests:
            prompt = ("\n*** Some tests are missing in Evergreen configuration ***\nPlease\n" +
                      "\t1) Run '{prog} generate' to generate and apply the Evergreen changes.\n" +
                      "\t2) Run 'git diff' to see the detail of the Evergreen changes.\n" +
                      "\t3) Trigger Evergreen patch build to verify the changes before merging.\n"
                     ).format(prog=prog)
            print(prompt)
            sys.exit(1)

    elif action == 'generate':
        if test_type in TEST_TYPES:
            missing_tests = check_missing_tests(test_type)
            generate_evg_cfg_for_missing_tests(test_type, missing_tests)
        elif test_type == 'all':
            # Check each of the test type
            for t in TEST_TYPES:
                missing_tests = check_missing_tests(t)
                generate_evg_cfg_for_missing_tests(t, missing_tests)
    else:
        sys.exit("Unsupported action type '%s'" % action)

if __name__ == '__main__':

    args = docopt.docopt(USAGE, version=DESCRIPTION)

    verbose = args['-v']
    debug('\nargs:%s' % args)

    action = None
    if args['check']:
        action = 'check'
    elif args['generate']:
        action = 'generate'
    assert action in ('check', 'generate')

    test_type = args.get('-t', None)
    # If test type is not provided, assuming 'all' types need to be checked
    if test_type is None:
        test_type = 'all'

    evg_cfg(action, test_type)
