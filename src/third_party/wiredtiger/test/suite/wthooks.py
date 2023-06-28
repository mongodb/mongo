#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# [TEST_TAGS]
# ignored_file
# [END_TAGS]
#
# WiredTigerHookManager
#   Manage running of hooks
#
from __future__ import print_function

from importlib import import_module
from abc import ABC, abstractmethod
import wiredtiger, os

# Three kinds of hooks available:
HOOK_REPLACE = 1     # replace the call with the hook function
HOOK_NOTIFY = 2      # call the hook function after the function
HOOK_ARGS = 3        # transform the arg list before the call

# Print to /dev/tty for debugging, since anything extraneous to stdout/stderr will
# cause a test error.
def tty(message):
    from wttest import WiredTigerTestCase
    WiredTigerTestCase.tty(message)

################
# Hooks Overview
#
# Here are some useful terms to know, with some commentary for each.
#
#   API functions
#      potentially any WiredTiger API functions that a hook creator wishes to modify (like
#      Session.rename).  In Python most everything is an object.  Of course an instance of
#      "Session" is an object, but also the "Session" class itself is an object.  The Session.rename
#      function is also an object (of a certain form that can be called).  Also in Python,
#      attributes on an object don't have to be "pre-declared", they can be created at any time.
#      So it's easy to imagine assigning Session._rename_orig to be (the original value of)
#      Session.rename, and then assigning Session.rename to be some other function object, that
#      knows how to do something and then perhaps calls Session._rename_orig .  This is the
#      essence of the hook concept.
#
#  Hook Creator:
#      A way to attach a set of "behavior modifications" to various API functions.  More precisely,
#      a hook creator derives from WiredTigerHookCreator and sets up a number of "hook functions",
#      that are actions that are done either just before, after, or instead of, an API function.
#      A XxxxHookCreator lives in a hook_xxxx.py file.  When a HookCreator is loaded, it may be
#      given an optional argument.  This argument comes from the original python command line.
#      For example, "python run.py --hook abc" loads hook_abc.py (where it expects to find a hook).
#      "python run.py --hook abc=123" loads hook_abc.py with an argument "123".
#
#  Hook Function:
#      One function that will be called before, after or instead of, an API function.  A hook
#      function will be bound to an API function.  It is the job of the HookCreator to set up that
#      binding.  It is possible to have multiple hook functions bound to the same API function.
#      A hook function that replaces an API function will have the same args as the function
#      it replaces (but there is a trick to give it additional context if needed -
#      see session_create_replace in hook_demo.py).
#
#  Hook Platform API:
#      A set of utility functions used by WiredTigerTestCase or other parts of the test framework
#      that may differ according to platform.  Rather than have hook specific implementations in the
#      test framework, the "platform API" is implemented by any hook that wants to override it.
#      Currently the hook specific implementation is all or nothing, in the future we may allow
#      subsets of the hook platform API to be implemented.


# For every API function altered, there is one of these objects
# stashed in the <class>._<api_name>_hooks attribute.
class WiredTigerHookInfo(object):
    def __init__(self):
        self.arg_funcs = []     # The set of hook functions for manipulating arguments
        self.notify_funcs = []    # The set of hook functions for manipulating arguments
        # At the moment, we can only replace a method once.
        # If needed, we can think about removing this restriction.
        self.replace_func = None

# hooked_function -
# A helper function for the hook manager.
def hooked_function(self, orig_func, hook_info_name, *args):
    hook_info = getattr(self, hook_info_name)

    notifies = []
    replace_func = None

    # The three kinds of hooks are acted upon at different times.
    # Before we call the function, we modify the args as indicated
    # by hooks.  Then we call the function, possibly with a replacement.
    # Finally, we'll call any notify hooks.
    #
    # We only walk through the hook list once, and process the config
    # hooks while we're doing that, and copy any other hooks needed.
    for hook_func in hook_info.arg_funcs:
        args = hook_func(self, args)
    call_func = hook_info.replace_func
    if call_func == None:
        call_func = orig_func
    if self == wiredtiger:
        ret = call_func(*args)
    else:
        ret = call_func(self, *args)
    for hook_func in hook_info.notify_funcs:
        hook_func(ret, self, *args)
    return ret

# WiredTigerHookManager -
# The hook manager class.  There is only one hook manager.  It is responsible for finding all the
# HookCreators at the beginning of the run, and calling setup_hooks() for each one, to have it bind
# hook functions to API functions.  The hook manager is initialized with a list of hook names. Each
# name is expanded, for example, "demo" causes the hook manager to load hook_demo.py, and to call
# the "initialize" global function in that file.  We expect "initialize" to return a list of objects
# (hooks) derived from WiredTigerHook (class defined below).  Generally, "initialize" returns a
# single object (setting up some number of "hook functions") but to allow flexibility for different
# sorts of packaging, we allow any number of hooks to be returned.
#
# A hook can set up any number of "hook functions".  See hook_demo.py for a sample hook class.
class WiredTigerHookManager(object):
    def __init__(self, hooknames = []):
        self.hooks = []
        self.platform_apis = []
        names_seen = []
        for name in hooknames:
            # The hooks are indicated as "somename=arg" or simply "somename".
            # hook_somename.py will be imported, and initialized with the arg.
            # Names must be unique, as we stash some info into extra fields
            # on the connection/session/cursor, these are named using the
            # unique name of the hook.
            if '=' in name:
                name,arg = name.split('=', 1)
            else:
                arg = None
            if name in names_seen:
                raise Exception(name + ': hook name cannot be used multiple times')
            names_seen.append(name)

            modname = 'hook_' + name
            try:
                imported = import_module(modname)
                for hook in imported.initialize(arg):
                    hook._initialize(name, self)
                    self.hooks.append(hook)
            except:
                print('Cannot import hook: ' + name + ', check file ' + modname + '.py')
                raise
        self.hook_names = tuple(names_seen)
        for hook in self.hooks:
            hook.setup_hooks()
            api = hook.get_platform_api()   # can return None
            self.platform_apis.append(api)
        self.platform_apis.append(DefaultPlatformAPI())

    def add_hook(self, clazz, method_name, hook_type, hook_func):
        if not hasattr(clazz, method_name):
            raise Exception('Cannot find method ' + method_name + ' on class ' + str(clazz))

        # We need to set up some extra attributes on the Connection class.
        # Given that the method name is XXXX, and class is Connection, here's what we're doing:
        #    orig = wiredtiger.Connection.XXXX
        #    wiredtiger.Connection._XXXX_hooks = WiredTigerHookInfo()
        #    wiredtiger.Connection._XXXX_orig = wiredtiger.Connection.XXXX
        #    wiredtiger.Connection.XXXX = lambda self, *args:
        #              hooked_function(self, orig, '_XXXX_hooks', *args)
        hook_info_name = '_' + method_name + '_hooks'
        orig_name = '_' + method_name + '_orig'
        if not hasattr(clazz, hook_info_name):
            #tty('Setting up hook on ' + str(clazz) + '.' + method_name)
            orig_func = getattr(clazz, method_name)
            if orig_func == None:
                raise Exception('method ' + method_name + ' hook setup: method does not exist')
            setattr(clazz, hook_info_name, WiredTigerHookInfo())

            # If we're using the wiredtiger module and not a class, we need a slightly different
            # style of hooked_function, since there is no self.  What would be the "self" argument
            # is in fact the class.
            if clazz == wiredtiger:
                f = lambda *args: hooked_function(wiredtiger, orig_func, hook_info_name, *args)
            else:
                f = lambda self, *args: hooked_function(self, orig_func, hook_info_name, *args)
            setattr(clazz, method_name, f)
            setattr(clazz, orig_name, orig_func)

        # Now add to the list of hook functions
        # If it's a replace hook, we only allow one of them for a given method name
        hook_info = getattr(clazz, hook_info_name)
        if hook_type == HOOK_ARGS:
            hook_info.arg_funcs.append(hook_func)
        elif hook_type == HOOK_NOTIFY:
            hook_info.notify_funcs.append(hook_func)
        elif hook_type == HOOK_REPLACE:
            if hook_info.replace_func == None:
                hook_info.replace_func = hook_func
            else:
                raise Exception('method ' + method_name + ' hook setup: trying to replace the same method with two hooks')
        #tty('Setting up hooks list in ' + str(clazz) + '.' + hook_info_name)

    def get_function(self, clazz, method_name):
        orig_name = '_' + method_name + '_orig'
        if hasattr(clazz, orig_name):
            orig_func = getattr(clazz, orig_name)
        else:
            orig_func = getattr(clazz, method_name)
        return orig_func

    def filter_tests(self, tests):
        for hook in self.hooks:
            tests = hook.filter_tests(tests)
        return tests

    def get_hook_names(self):
        return self.hook_names

    def get_platform_api(self):
        return MultiPlatformAPI(self.platform_apis)

    # Returns a list of hook names that use something on the list
    def hooks_using(self, use_list):
        ret = []
        for hook in self.hooks:
            if hook.uses(use_list):
                ret.append(hook.name)
        return ret

class HookCreatorProxy(object):
    def __init__(self, hookmgr, clazz):
        self.hookmgr = hookmgr
        self.clazz = clazz

    # Get the original function/method before any hooks applied
    def __getitem__(self, name):
        return self.hookmgr.get_function(self.clazz, name)

    # Get the original function/method before any hooks applied
    def __setitem__(self, name, value):
        try:
            hooktype = int(value[0])
            fcn = value[1]
        except:
            raise ValueError('value must be (HOOK_xxxx, function)')
        self.hookmgr.add_hook(self.clazz, name, hooktype, fcn)

# Hooks must derive from this class
class WiredTigerHookCreator(ABC):
    # This is called right after creation and should not be overridden.
    def _initialize(self, name, hookmgr):
        self.name = name
        self.hookmgr = hookmgr
        self.wiredtiger = HookCreatorProxy(self.hookmgr, wiredtiger)
        self.Connection = HookCreatorProxy(self.hookmgr, wiredtiger.Connection)
        self.Session = HookCreatorProxy(self.hookmgr, wiredtiger.Session)
        self.Cursor = HookCreatorProxy(self.hookmgr, wiredtiger.Cursor)

    # default version of filter_tests, can be overridden
    def filter_tests(self, tests):
        return tests

    @abstractmethod
    def setup_hooks(self):
        """Set up all hooks using add_*_hook methods."""
        return

    # default version of uses, can be overridden.  If the hook uses or provides
    # a capability on the list, it should return True.
    def uses(self, use_list):
        return False

class WiredTigerHookPlatformAPI(object):
    def setUp(self):
        """Called at the beginning of a test case"""
        pass

    def tearDown(self):
        """Called at the termination of a test case"""
        pass

    def tableExists(self, name):
        """Return boolean if local files exist for the table with the given base name"""
        raise NotImplementedError('tableExists method not implemented')

    def initialFileName(self, uri):
        """The first local backing file name created for this URI."""
        raise NotImplementedError('initialFileName method not implemented')

    def getTimestamp(self):
        """The timestamp generator for this test case."""
        raise NotImplementedError('getTimestamp method not implemented')

    def getTierSharePercent(self):
        """The tier share percentage generator for this test case."""
        raise NotImplementedError('getTierSharePercent method not implemented')

    def getTierCachePercent(self):
        """The tier cache percentage generator for this test case."""
        raise NotImplementedError('getTierCachePercent method not implemented')

    def getTierStorageSource(self):
        """The tier cache percentage generator for this test case."""
        raise NotImplementedError('getTierStorageSource method not implemented')

class DefaultPlatformAPI(WiredTigerHookPlatformAPI):
    def tableExists(self, name):
        tablename = name + ".wt"
        return os.path.exists(tablename)

    def initialFileName(self, uri):
        if uri.startswith('table:'):
            return uri[6:] + '.wt'
        elif uri.startswith('file:'):
            return uri[5:]
        else:
            raise Exception('bad uri')

    # By default, there is no automatic timestamping by test infrastructure classes.
    def getTimestamp(self):
        return None

    # By default, all the populated data lies in the local storage.
    def getTierSharePercent(self):
        return 0

    # By default, all the populated data lies in the cache.
    def getTierCachePercent(self):
        return 0

    # By default, dir_store is the storage source.
    def getTierStorageSource(self):
        return ('dir_store')

class MultiPlatformAPI(WiredTigerHookPlatformAPI):
    def __init__(self, platform_apis):
        self.apis = platform_apis

    def setUp(self):
        """Called at the beginning of a test case"""
        for api in self.apis:
            api.setUp()

    def tearDown(self):
        """Called at the termination of a test case"""
        for api in self.apis:
            api.tearDown()

    def tableExists(self, name):
        """Return boolean if local files exist for the table with the given base name"""
        for api in self.apis:
            try:
                return api.tableExists(name)
            except NotImplementedError:
                pass
        raise Exception('tableExists: no implementation')  # should never happen

    def initialFileName(self, uri):
        """The first local backing file name created for this URI."""
        for api in self.apis:
            try:
                return api.initialFileName(uri)
            except NotImplementedError:
                pass
        raise Exception('initialFileName: no implementation')  # should never happen

    def getTimestamp(self):
        """The timestamp generator for this test case."""
        for api in self.apis:
            try:
                return api.getTimestamp()
            except NotImplementedError:
                pass
        raise Exception('getTimestamp: no implementation')  # should never happen

    def getTierSharePercent(self):
        """The tier share value for this test case."""
        for api in self.apis:
            try:
                return api.getTierSharePercent()
            except NotImplementedError:
                pass
        raise Exception('getTierSharePercent: no implementation')  # should never happen

    def getTierCachePercent(self):
        """The tier cache value for this test case."""
        for api in self.apis:
            try:
                return api.getTierCachePercent()
            except NotImplementedError:
                pass
        raise Exception('getTierCachePercent: no implementation')  # should never happen

    def getTierStorageSource(self):
        """The tier storage source for this test case."""
        for api in self.apis:
            try:
                return api.getTierStorageSource()
            except NotImplementedError:
                pass
        raise Exception('getTierStorageSource: no implementation')  # should never happen
