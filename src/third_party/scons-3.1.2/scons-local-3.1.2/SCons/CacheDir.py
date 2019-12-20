#
# Copyright (c) 2001 - 2019 The SCons Foundation
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

__revision__ = "src/engine/SCons/CacheDir.py bee7caf9defd6e108fc2998a2520ddb36a967691 2019-12-17 02:07:09 bdeegan"

__doc__ = """
CacheDir support
"""

import hashlib
import json
import os
import stat
import sys

import SCons
import SCons.Action
import SCons.Warnings
from SCons.Util import PY3

cache_enabled = True
cache_debug = False
cache_force = False
cache_show = False
cache_readonly = False

def CacheRetrieveFunc(target, source, env):
    t = target[0]
    fs = t.fs
    cd = env.get_CacheDir()
    cd.requests += 1
    cachedir, cachefile = cd.cachepath(t)
    if not fs.exists(cachefile):
        cd.CacheDebug('CacheRetrieve(%s):  %s not in cache\n', t, cachefile)
        return 1
    cd.hits += 1
    cd.CacheDebug('CacheRetrieve(%s):  retrieving from %s\n', t, cachefile)
    if SCons.Action.execute_actions:
        if fs.islink(cachefile):
            fs.symlink(fs.readlink(cachefile), t.get_internal_path())
        else:
            env.copy_from_cache(cachefile, t.get_internal_path())
            try:
                os.utime(cachefile, None)
            except OSError:
                pass
        st = fs.stat(cachefile)
        fs.chmod(t.get_internal_path(), stat.S_IMODE(st[stat.ST_MODE]) | stat.S_IWRITE)
    return 0

def CacheRetrieveString(target, source, env):
    t = target[0]
    fs = t.fs
    cd = env.get_CacheDir()
    cachedir, cachefile = cd.cachepath(t)
    if t.fs.exists(cachefile):
        return "Retrieved `%s' from cache" % t.get_internal_path()
    return None

CacheRetrieve = SCons.Action.Action(CacheRetrieveFunc, CacheRetrieveString)

CacheRetrieveSilent = SCons.Action.Action(CacheRetrieveFunc, None)

def CachePushFunc(target, source, env):
    if cache_readonly:
        return

    t = target[0]
    if t.nocache:
        return
    fs = t.fs
    cd = env.get_CacheDir()
    cachedir, cachefile = cd.cachepath(t)
    if fs.exists(cachefile):
        # Don't bother copying it if it's already there.  Note that
        # usually this "shouldn't happen" because if the file already
        # existed in cache, we'd have retrieved the file from there,
        # not built it.  This can happen, though, in a race, if some
        # other person running the same build pushes their copy to
        # the cache after we decide we need to build it but before our
        # build completes.
        cd.CacheDebug('CachePush(%s):  %s already exists in cache\n', t, cachefile)
        return

    cd.CacheDebug('CachePush(%s):  pushing to %s\n', t, cachefile)

    tempfile = cachefile+'.tmp'+str(os.getpid())
    errfmt = "Unable to copy %s to cache. Cache file is %s"

    if not fs.isdir(cachedir):
        try:
            fs.makedirs(cachedir)
        except EnvironmentError:
            # We may have received an exception because another process
            # has beaten us creating the directory.
            if not fs.isdir(cachedir):
                msg = errfmt % (str(target), cachefile)
                raise SCons.Errors.SConsEnvironmentError(msg)

    try:
        if fs.islink(t.get_internal_path()):
            fs.symlink(fs.readlink(t.get_internal_path()), tempfile)
        else:
            fs.copy2(t.get_internal_path(), tempfile)
        fs.rename(tempfile, cachefile)
        st = fs.stat(t.get_internal_path())
        fs.chmod(cachefile, stat.S_IMODE(st[stat.ST_MODE]) | stat.S_IWRITE)
    except EnvironmentError:
        # It's possible someone else tried writing the file at the
        # same time we did, or else that there was some problem like
        # the CacheDir being on a separate file system that's full.
        # In any case, inability to push a file to cache doesn't affect
        # the correctness of the build, so just print a warning.
        msg = errfmt % (str(target), cachefile)
        SCons.Warnings.warn(SCons.Warnings.CacheWriteErrorWarning, msg)

CachePush = SCons.Action.Action(CachePushFunc, None)

# Nasty hack to cut down to one warning for each cachedir path that needs
# upgrading.
warned = dict()

class CacheDir(object):

    def __init__(self, path):
        """
        Initialize a CacheDir object.

        The cache configuration is stored in the object. It
        is read from the config file in the supplied path if
        one exists,  if not the config file is created and
        the default config is written, as well as saved in the object.
        """
        self.requests = 0
        self.hits = 0
        self.path = path
        self.current_cache_debug = None
        self.debugFP = None
        self.config = dict()
        if path is None:
            return

        if PY3:
            self._readconfig3(path)
        else:
            self._readconfig2(path)


    def _readconfig3(self, path):
        """
        Python3 version of reading the cache config.

        If directory or config file do not exist, create.  Take advantage
        of Py3 capability in os.makedirs() and in file open(): just try
        the operation and handle failure appropriately.

        Omit the check for old cache format, assume that's old enough
        there will be none of those left to worry about.

        :param path: path to the cache directory
        """
        config_file = os.path.join(path, 'config')
        try:
            os.makedirs(path, exist_ok=True)
        except FileExistsError:
            pass
        except OSError:
            msg = "Failed to create cache directory " + path
            raise SCons.Errors.SConsEnvironmentError(msg)

        try:
            with open(config_file, 'x') as config:
                self.config['prefix_len'] = 2
                try:
                    json.dump(self.config, config)
                except Exception:
                    msg = "Failed to write cache configuration for " + path
                    raise SCons.Errors.SConsEnvironmentError(msg)
        except FileExistsError:
            try:
                with open(config_file) as config:
                    self.config = json.load(config)
            except ValueError:
                msg = "Failed to read cache configuration for " + path
                raise SCons.Errors.SConsEnvironmentError(msg)


    def _readconfig2(self, path):
        """
        Python2 version of reading cache config.

        See if there is a config file in the cache directory. If there is,
        use it. If there isn't, and the directory exists and isn't empty,
        produce a warning. If the directory does not exist or is empty,
        write a config file.

        :param path: path to the cache directory
        """
        config_file = os.path.join(path, 'config')
        if not os.path.exists(config_file):
            # A note: There is a race hazard here if two processes start and
            # attempt to create the cache directory at the same time. However,
            # Python 2.x does not give you the option to do exclusive file
            # creation (not even the option to error on opening an existing
            # file for writing...). The ordering of events here is an attempt
            # to alleviate this, on the basis that it's a pretty unlikely
            # occurrence (would require two builds with a brand new cache
            # directory)
            if os.path.isdir(path) and any(f != "config" for f in os.listdir(path)):
                self.config['prefix_len'] = 1
                # When building the project I was testing this on, the warning
                # was output over 20 times. That seems excessive
                global warned
                if self.path not in warned:
                    msg = "Please upgrade your cache by running " +\
                          "scons-configure-cache.py " +  self.path
                    SCons.Warnings.warn(SCons.Warnings.CacheVersionWarning, msg)
                    warned[self.path] = True
            else:
                if not os.path.isdir(path):
                    try:
                        os.makedirs(path)
                    except OSError:
                        # If someone else is trying to create the directory at
                        # the same time as me, bad things will happen
                        msg = "Failed to create cache directory " + path
                        raise SCons.Errors.SConsEnvironmentError(msg)

                self.config['prefix_len'] = 2
                if not os.path.exists(config_file):
                    try:
                        with open(config_file, 'w') as config:
                            json.dump(self.config, config)
                    except Exception:
                        msg = "Failed to write cache configuration for " + path
                        raise SCons.Errors.SConsEnvironmentError(msg)
        else:
            try:
                with open(config_file) as config:
                    self.config = json.load(config)
            except ValueError:
                msg = "Failed to read cache configuration for " + path
                raise SCons.Errors.SConsEnvironmentError(msg)


    def CacheDebug(self, fmt, target, cachefile):
        if cache_debug != self.current_cache_debug:
            if cache_debug == '-':
                self.debugFP = sys.stdout
            elif cache_debug:
                self.debugFP = open(cache_debug, 'w')
            else:
                self.debugFP = None
            self.current_cache_debug = cache_debug
        if self.debugFP:
            self.debugFP.write(fmt % (target, os.path.split(cachefile)[1]))
            self.debugFP.write("requests: %d, hits: %d, misses: %d, hit rate: %.2f%%\n" %
                               (self.requests, self.hits, self.misses, self.hit_ratio))

    @property
    def hit_ratio(self):
        return (100.0 * self.hits / self.requests if self.requests > 0 else 100)

    @property
    def misses(self):
        return self.requests - self.hits

    def is_enabled(self):
        return cache_enabled and self.path is not None

    def is_readonly(self):
        return cache_readonly

    def cachepath(self, node):
        """
        """
        if not self.is_enabled():
            return None, None

        sig = node.get_cachedir_bsig()

        subdir = sig[:self.config['prefix_len']].upper()

        dir = os.path.join(self.path, subdir)
        return dir, os.path.join(dir, sig)

    def retrieve(self, node):
        """
        This method is called from multiple threads in a parallel build,
        so only do thread safe stuff here. Do thread unsafe stuff in
        built().

        Note that there's a special trick here with the execute flag
        (one that's not normally done for other actions).  Basically
        if the user requested a no_exec (-n) build, then
        SCons.Action.execute_actions is set to 0 and when any action
        is called, it does its showing but then just returns zero
        instead of actually calling the action execution operation.
        The problem for caching is that if the file does NOT exist in
        cache then the CacheRetrieveString won't return anything to
        show for the task, but the Action.__call__ won't call
        CacheRetrieveFunc; instead it just returns zero, which makes
        the code below think that the file *was* successfully
        retrieved from the cache, therefore it doesn't do any
        subsequent building.  However, the CacheRetrieveString didn't
        print anything because it didn't actually exist in the cache,
        and no more build actions will be performed, so the user just
        sees nothing.  The fix is to tell Action.__call__ to always
        execute the CacheRetrieveFunc and then have the latter
        explicitly check SCons.Action.execute_actions itself.
        """
        if not self.is_enabled():
            return False

        env = node.get_build_env()
        if cache_show:
            if CacheRetrieveSilent(node, [], env, execute=1) == 0:
                node.build(presub=0, execute=0)
                return True
        else:
            if CacheRetrieve(node, [], env, execute=1) == 0:
                return True

        return False

    def push(self, node):
        if self.is_readonly() or not self.is_enabled():
            return
        return CachePush(node, [], node.get_build_env())

    def push_if_forced(self, node):
        if cache_force:
            return self.push(node)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
