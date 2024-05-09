#! /usr/bin/env python
# Copyright Michael Foord 2009-2010
# discover.py
# Test discovery for unittest
# Compatible with Python 2.4-2.6 and 3.0-3.1
# Licensed under the BSD License
# See: http://pypi.python.org/pypi/discover

import optparse
import os
import re
import sys
import traceback
import types
import unittest

from fnmatch import fnmatch

__version__ = '0.4.0'
__all__ = ['DiscoveringTestLoader', 'main', 'defaultTestLoader']


if hasattr(types, 'ClassType'):
    class_types = (types.ClassType, type)
else:
    # for Python 3.0 compatibility
    class_types = type

def _CmpToKey(mycmp):
    'Convert a cmp= function into a key= function'
    class K(object):
        def __init__(self, obj):
            self.obj = obj
        def __lt__(self, other):
            return mycmp(self.obj, other.obj) == -1
    return K

try:
    from types import UnboundMethodType
except ImportError:
    # Python 3 compatibility
    UnboundMethodType = types.FunctionType

# what about .pyc or .pyo (etc)
# we would need to avoid loading the same tests multiple times
# from '.py', '.pyc' *and* '.pyo'
VALID_MODULE_NAME = re.compile(r'[_a-z]\w*\.py$', re.IGNORECASE)


def _make_failed_import_test(name, suiteClass):
    message = 'Failed to import test module: %s' % name
    if hasattr(traceback, 'format_exc'):
        # Python 2.3 compatibility
        # format_exc returns two frames of discover.py as well
        message += '\n%s' % traceback.format_exc()
    return _make_failed_test('ModuleImportFailure', name, ImportError(message),
                             suiteClass)

def _make_failed_load_tests(name, exception, suiteClass):
    return _make_failed_test('LoadTestsFailure', name, exception, suiteClass)

def _make_failed_test(classname, methodname, exception, suiteClass):
    def testFailure(self):
        raise exception
    attrs = {methodname: testFailure}
    TestClass = type(classname, (unittest.TestCase,), attrs)
    return suiteClass((TestClass(methodname),))

try:
    cmp
except NameError:
    @staticmethod
    def cmp(x, y):
        """Return -1 if x < y, 0 if x == y and 1 if x > y"""
        return (x > y) - (x < y)


class DiscoveringTestLoader(unittest.TestLoader):
    """
    This class is responsible for loading tests according to various criteria
    and returning them wrapped in a TestSuite
    """
    testMethodPrefix = 'test'
    sortTestMethodsUsing = cmp
    suiteClass = unittest.TestSuite
    _top_level_dir = None

    def loadTestsFromTestCase(self, testCaseClass):
        """Return a suite of all tests cases contained in testCaseClass"""
        if issubclass(testCaseClass, unittest.TestSuite):
            raise TypeError("Test cases should not be derived from TestSuite."
                            " Maybe you meant to derive from TestCase?")
        testCaseNames = self.getTestCaseNames(testCaseClass)
        if not testCaseNames and hasattr(testCaseClass, 'runTest'):
            testCaseNames = ['runTest']
        loaded_suite = self.suiteClass(map(testCaseClass, testCaseNames))
        return loaded_suite

    def loadTestsFromModule(self, module, use_load_tests=True):
        """Return a suite of all tests cases contained in the given module"""
        tests = []
        for name in dir(module):
            obj = getattr(module, name)
            if isinstance(obj, type) and issubclass(obj, unittest.TestCase):
                tests.append(self.loadTestsFromTestCase(obj))

        load_tests = getattr(module, 'load_tests', None)
        tests = self.suiteClass(tests)
        if use_load_tests and load_tests is not None:
            try:
                return load_tests(self, tests, None)
            except:
                ExceptionClass, e = sys.exc_info()[:2]
                if not isinstance(e, Exception):
                    # for BaseException exceptions
                    raise
                return _make_failed_load_tests(module.__name__, e,
                                               self.suiteClass)
        return tests

    def loadTestsFromName(self, name, module=None):
        """Return a suite of all tests cases given a string specifier.

        The name may resolve either to a module, a test case class, a
        test method within a test case class, or a callable object which
        returns a TestCase or TestSuite instance.

        The method optionally resolves the names relative to a given module.
        """
        parts = name.split('.')
        if module is None:
            parts_copy = parts[:]
            while parts_copy:
                try:
                    module = __import__('.'.join(parts_copy))
                    break
                except ImportError:
                    del parts_copy[-1]
                    if not parts_copy:
                        raise
            parts = parts[1:]
        obj = module
        for part in parts:
            parent, obj = obj, getattr(obj, part)

        if isinstance(obj, types.ModuleType):
            return self.loadTestsFromModule(obj)
        elif isinstance(obj, type) and issubclass(obj, unittest.TestCase):
            return self.loadTestsFromTestCase(obj)
        elif (isinstance(obj, UnboundMethodType) and
              isinstance(parent, type) and
              issubclass(parent, unittest.TestCase)):
            name = obj.__name__
            inst = parent(name)
            # static methods follow a different path
            if not isinstance(getattr(inst, name), types.FunctionType):
                return self.suiteClass([inst])
        elif isinstance(obj, unittest.TestSuite):
            return obj
        if hasattr(obj, '__call__'):
            test = obj()
            if isinstance(test, unittest.TestSuite):
                return test
            elif isinstance(test, unittest.TestCase):
                return self.suiteClass([test])
            else:
                raise TypeError("calling %s returned %s, not a test" %
                                (obj, test))
        else:
            raise TypeError("don't know how to make test from: %s" % obj)

    def loadTestsFromNames(self, names, module=None):
        """Return a suite of all tests cases found using the given sequence
        of string specifiers. See 'loadTestsFromName()'.
        """
        suites = [self.loadTestsFromName(name, module) for name in names]
        return self.suiteClass(suites)

    def getTestCaseNames(self, testCaseClass):
        """Return a sorted sequence of method names found within testCaseClass
        """
        def isTestMethod(attrname, testCaseClass=testCaseClass,
                         prefix=self.testMethodPrefix):
            return attrname.startswith(prefix) and \
                hasattr(getattr(testCaseClass, attrname), '__call__')
        testFnNames = list(filter(isTestMethod, dir(testCaseClass)))
        if self.sortTestMethodsUsing:
            testFnNames.sort(key=_CmpToKey(self.sortTestMethodsUsing))
        return testFnNames

    def discover(self, start_dir, pattern='test*.py', top_level_dir=None):
        """Find and return all test modules from the specified start
        directory, recursing into subdirectories to find them. Only test files
        that match the pattern will be loaded. (Using shell style pattern
        matching.)

        All test modules must be importable from the top level of the project.
        If the start directory is not the top level directory then the top
        level directory must be specified separately.

        If a test package name (directory with '__init__.py') matches the
        pattern then the package will be checked for a 'load_tests' function. If
        this exists then it will be called with loader, tests, pattern.

        If load_tests exists then discovery does  *not* recurse into the package,
        load_tests is responsible for loading all tests in the package.

        The pattern is deliberately not stored as a loader attribute so that
        packages can continue discovery themselves. top_level_dir is stored so
        load_tests does not need to pass this argument in to loader.discover().
        """
        set_implicit_top = False
        if top_level_dir is None and self._top_level_dir is not None:
            # make top_level_dir optional if called from load_tests in a package
            top_level_dir = self._top_level_dir
        elif top_level_dir is None:
            set_implicit_top = True
            top_level_dir = start_dir

        top_level_dir = os.path.abspath(top_level_dir)

        if not top_level_dir in sys.path:
            # all test modules must be importable from the top level directory
            # should we *unconditionally* put the start directory in first
            # in sys.path to minimise likelihood of conflicts between installed
            # modules and development versions?
            sys.path.insert(0, top_level_dir)
        self._top_level_dir = top_level_dir

        is_not_importable = False
        if os.path.isdir(os.path.abspath(start_dir)):
            start_dir = os.path.abspath(start_dir)
            if start_dir != top_level_dir:
                is_not_importable = not os.path.isfile(os.path.join(start_dir, '__init__.py'))
        else:
            # support for discovery from dotted module names
            try:
                __import__(start_dir)
            except ImportError:
                is_not_importable = True
            else:
                the_module = sys.modules[start_dir]
                top_part = start_dir.split('.')[0]
                start_dir = os.path.abspath(os.path.dirname((the_module.__file__)))
                if set_implicit_top:
                    self._top_level_dir = os.path.abspath(os.path.dirname(os.path.dirname(sys.modules[top_part].__file__)))
                    sys.path.remove(top_level_dir)

        if is_not_importable:
            raise ImportError('Start directory is not importable: %r' % start_dir)

        tests = list(self._find_tests(start_dir, pattern))
        return self.suiteClass(tests)

    def _get_name_from_path(self, path):
        path = os.path.splitext(os.path.normpath(path))[0]

        _relpath = relpath(path, self._top_level_dir)
        assert not os.path.isabs(_relpath), "Path must be within the project"
        assert not _relpath.startswith('..'), "Path must be within the project"

        name = _relpath.replace(os.path.sep, '.')
        return name

    def _get_module_from_name(self, name):
        __import__(name)
        return sys.modules[name]

    def _match_path(self, path, full_path, pattern):
        # override this method to use alternative matching strategy
        return fnmatch(path, pattern)
    
    def _find_tests(self, start_dir, pattern):
        """Used by discovery. Yields test suites it loads."""
        paths = os.listdir(start_dir)

        for path in paths:
            full_path = os.path.join(start_dir, path)
            if os.path.isfile(full_path):
                if not VALID_MODULE_NAME.match(path):
                    # valid Python identifiers only
                    continue
                if not self._match_path(path, full_path, pattern):
                    continue
                # if the test file matches, load it
                name = self._get_name_from_path(full_path)
                try:
                    module = self._get_module_from_name(name)
                except:
                    yield _make_failed_import_test(name, self.suiteClass)
                else:
                    mod_file = os.path.abspath(getattr(module, '__file__', full_path))
                    realpath = os.path.splitext(mod_file)[0]
                    fullpath_noext = os.path.splitext(full_path)[0]
                    if realpath.lower() != fullpath_noext.lower():
                        module_dir = os.path.dirname(realpath)
                        mod_name = os.path.splitext(os.path.basename(full_path))[0]
                        expected_dir = os.path.dirname(full_path)
                        msg = ("%r module incorrectly imported from %r. Expected %r. "
                               "Is this module globally installed?")
                        raise ImportError(msg % (mod_name, module_dir, expected_dir))
                    yield self.loadTestsFromModule(module)
            elif os.path.isdir(full_path):
                if not os.path.isfile(os.path.join(full_path, '__init__.py')):
                    continue

                load_tests = None
                tests = None
                if fnmatch(path, pattern):
                    # only check load_tests if the package directory itself matches the filter
                    name = self._get_name_from_path(full_path)
                    package = self._get_module_from_name(name)
                    load_tests = getattr(package, 'load_tests', None)
                    tests = self.loadTestsFromModule(package, use_load_tests=False)

                if load_tests is None:
                    if tests is not None:
                        # tests loaded from package file
                        yield tests
                    # recurse into the package
                    for test in self._find_tests(full_path, pattern):
                        yield test
                else:
                    try:
                        yield load_tests(self, tests, pattern)
                    except:
                        ExceptionClass, e = sys.exc_info()[:2]
                        if not isinstance(e, Exception):
                            # for BaseException exceptions
                            raise
                        yield _make_failed_load_tests(package.__name__, e,
                                                      self.suiteClass)


##############################################
# relpath implementation taken from Python 2.7

if not hasattr(os.path, 'relpath'):
    if os.path is sys.modules.get('ntpath'):
        def relpath(path, start=os.path.curdir):
            """Return a relative version of a path"""
        
            if not path:
                raise ValueError("no path specified")
            start_list = os.path.abspath(start).split(os.path.sep)
            path_list = os.path.abspath(path).split(os.path.sep)
            if start_list[0].lower() != path_list[0].lower():
                unc_path, rest = os.path.splitunc(path)
                unc_start, rest = os.path.splitunc(start)
                if bool(unc_path) ^ bool(unc_start):
                    raise ValueError("Cannot mix UNC and non-UNC paths (%s and %s)"
                                                                        % (path, start))
                else:
                    raise ValueError("path is on drive %s, start on drive %s"
                                                        % (path_list[0], start_list[0]))
            # Work out how much of the filepath is shared by start and path.
            for i in range(min(len(start_list), len(path_list))):
                if start_list[i].lower() != path_list[i].lower():
                    break
            else:
                i += 1
        
            rel_list = [os.path.pardir] * (len(start_list)-i) + path_list[i:]
            if not rel_list:
                return os.path.curdir
            return os.path.join(*rel_list)
    
    else:
        # default to posixpath definition
        def relpath(path, start=os.path.curdir):
            """Return a relative version of a path"""
        
            if not path:
                raise ValueError("no path specified")
        
            start_list = os.path.abspath(start).split(os.path.sep)
            path_list = os.path.abspath(path).split(os.path.sep)
        
            # Work out how much of the filepath is shared by start and path.
            i = len(os.path.commonprefix([start_list, path_list]))
        
            rel_list = [os.path.pardir] * (len(start_list)-i) + path_list[i:]
            if not rel_list:
                return os.path.curdir
            return os.path.join(*rel_list)
else:
    from os.path import relpath

#############################################


USAGE = """\
Usage: discover.py [options]

Options:
  -v, --verbose    Verbose output
  -s directory     Directory to start discovery ('.' default)
  -p pattern       Pattern to match test files ('test*.py' default)
  -t directory     Top level directory of project (default to
                   start directory)

For test discovery all test modules must be importable from the top
level directory of the project.
"""

def _usage_exit(msg=None):
    if msg:
        print (msg)
    print (USAGE)
    sys.exit(2)


def _do_discovery(argv, verbosity, Loader):
    # handle command line args for test discovery
    parser = optparse.OptionParser()
    parser.add_option('-v', '--verbose', dest='verbose', default=False,
                      help='Verbose output', action='store_true')
    parser.add_option('-s', '--start-directory', dest='start', default='.',
                      help="Directory to start discovery ('.' default)")
    parser.add_option('-p', '--pattern', dest='pattern', default='test*.py',
                      help="Pattern to match tests ('test*.py' default)")
    parser.add_option('-t', '--top-level-directory', dest='top', default=None,
                      help='Top level directory of project (defaults to start directory)')

    options, args = parser.parse_args(argv)
    if len(args) > 3:
        _usage_exit()

    for name, value in zip(('start', 'pattern', 'top'), args):
        setattr(options, name, value)

    if options.verbose:
        verbosity = 2

    start_dir = options.start
    pattern = options.pattern
    top_level_dir = options.top

    loader = Loader()
    return loader.discover(start_dir, pattern, top_level_dir), verbosity


def _run_tests(tests, testRunner, verbosity, exit):
    if isinstance(testRunner, class_types):
        try:
            testRunner = testRunner(verbosity=verbosity)
        except TypeError:
            # didn't accept the verbosity argument
            testRunner = testRunner()
    result = testRunner.run(tests)
    if exit:
        sys.exit(not result.wasSuccessful())
    return result


def main(argv=None, testRunner=None, testLoader=None, exit=True, verbosity=1):
    if testLoader is None:
        testLoader = DiscoveringTestLoader
    if testRunner is None:
        testRunner = unittest.TextTestRunner
    if argv is None:
        argv = sys.argv[1:]

    tests, verbosity = _do_discovery(argv, verbosity, testLoader)
    return _run_tests(tests, testRunner, verbosity, exit)

defaultTestLoader = DiscoveringTestLoader()

def collector():
    # import __main__ triggers code re-execution
    __main__ = sys.modules['__main__']
    setupDir = os.path.abspath(os.path.dirname(__main__.__file__))
    return defaultTestLoader.discover(setupDir)

if __name__ == '__main__':
    if sys.argv[0] is None:
        # fix for weird behaviour when run with python -m
        # from a zipped egg.
        sys.argv[0] = 'discover.py'
    main()
