# Copyright (c) 2010-2012 extras developers. See LICENSE for details.

from testtools import TestCase
from testtools.matchers import (
    Equals,
    Is,
    Not,
    )

from extras import (
    safe_hasattr,
    try_import,
    try_imports,
    )

def check_error_callback(test, function, arg, expected_error_count,
    expect_result):
    """General test template for error_callback argument.

    :param test: Test case instance.
    :param function: Either try_import or try_imports.
    :param arg: Name or names to import.
    :param expected_error_count: Expected number of calls to the callback.
    :param expect_result: Boolean for whether a module should
        ultimately be returned or not.
    """
    cb_calls = []
    def cb(e):
        test.assertIsInstance(e, ImportError)
        cb_calls.append(e)
    try:
        result = function(arg, error_callback=cb)
    except ImportError:
        test.assertFalse(expect_result)
    else:
        if expect_result:
            test.assertThat(result, Not(Is(None)))
        else:
            test.assertThat(result, Is(None))
    test.assertEquals(len(cb_calls), expected_error_count)


class TestSafeHasattr(TestCase):

    def test_attribute_not_there(self):
        class Foo(object):
            pass
        self.assertEqual(False, safe_hasattr(Foo(), 'anything'))

    def test_attribute_there(self):
        class Foo(object):
            pass
        foo = Foo()
        foo.attribute = None
        self.assertEqual(True, safe_hasattr(foo, 'attribute'))

    def test_property_there(self):
        class Foo(object):
            @property
            def attribute(self):
                return None
        foo = Foo()
        self.assertEqual(True, safe_hasattr(foo, 'attribute'))

    def test_property_raises(self):
        class Foo(object):
            @property
            def attribute(self):
                1/0
        foo = Foo()
        self.assertRaises(ZeroDivisionError, safe_hasattr, foo, 'attribute')


class TestTryImport(TestCase):

    def test_doesnt_exist(self):
        # try_import('thing', foo) returns foo if 'thing' doesn't exist.
        marker = object()
        result = try_import('doesntexist', marker)
        self.assertThat(result, Is(marker))

    def test_None_is_default_alternative(self):
        # try_import('thing') returns None if 'thing' doesn't exist.
        result = try_import('doesntexist')
        self.assertThat(result, Is(None))

    def test_existing_module(self):
        # try_import('thing', foo) imports 'thing' and returns it if it's a
        # module that exists.
        result = try_import('os', object())
        import os
        self.assertThat(result, Is(os))

    def test_existing_submodule(self):
        # try_import('thing.another', foo) imports 'thing' and returns it if
        # it's a module that exists.
        result = try_import('os.path', object())
        import os
        self.assertThat(result, Is(os.path))

    def test_nonexistent_submodule(self):
        # try_import('thing.another', foo) imports 'thing' and returns foo if
        # 'another' doesn't exist.
        marker = object()
        result = try_import('os.doesntexist', marker)
        self.assertThat(result, Is(marker))

    def test_object_from_module(self):
        # try_import('thing.object') imports 'thing' and returns
        # 'thing.object' if 'thing' is a module and 'object' is not.
        result = try_import('os.path.join')
        import os
        self.assertThat(result, Is(os.path.join))

    def test_error_callback(self):
        # the error callback is called on failures.
        check_error_callback(self, try_import, 'doesntexist', 1, False)

    def test_error_callback_missing_module_member(self):
        # the error callback is called on failures to find an object
        # inside an existing module.
        check_error_callback(self, try_import, 'os.nonexistent', 1, False)

    def test_error_callback_not_on_success(self):
        # the error callback is not called on success.
        check_error_callback(self, try_import, 'os.path', 0, True)


class TestTryImports(TestCase):

    def test_doesnt_exist(self):
        # try_imports('thing', foo) returns foo if 'thing' doesn't exist.
        marker = object()
        result = try_imports(['doesntexist'], marker)
        self.assertThat(result, Is(marker))

    def test_fallback(self):
        result = try_imports(['doesntexist', 'os'])
        import os
        self.assertThat(result, Is(os))

    def test_None_is_default_alternative(self):
        # try_imports('thing') returns None if 'thing' doesn't exist.
        e = self.assertRaises(
            ImportError, try_imports, ['doesntexist', 'noreally'])
        self.assertThat(
            str(e),
            Equals("Could not import any of: doesntexist, noreally"))

    def test_existing_module(self):
        # try_imports('thing', foo) imports 'thing' and returns it if it's a
        # module that exists.
        result = try_imports(['os'], object())
        import os
        self.assertThat(result, Is(os))

    def test_existing_submodule(self):
        # try_imports('thing.another', foo) imports 'thing' and returns it if
        # it's a module that exists.
        result = try_imports(['os.path'], object())
        import os
        self.assertThat(result, Is(os.path))

    def test_nonexistent_submodule(self):
        # try_imports('thing.another', foo) imports 'thing' and returns foo if
        # 'another' doesn't exist.
        marker = object()
        result = try_imports(['os.doesntexist'], marker)
        self.assertThat(result, Is(marker))

    def test_fallback_submodule(self):
        result = try_imports(['os.doesntexist', 'os.path'])
        import os
        self.assertThat(result, Is(os.path))

    def test_error_callback(self):
        # One error for every class that doesn't exist.
        check_error_callback(self, try_imports,
            ['os.doesntexist', 'os.notthiseither'],
            2, False)
        check_error_callback(self, try_imports,
            ['os.doesntexist', 'os.notthiseither', 'os'],
            2, True)
        check_error_callback(self, try_imports,
            ['os.path'],
            0, True)


