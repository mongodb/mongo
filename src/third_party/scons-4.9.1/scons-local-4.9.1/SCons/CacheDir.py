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

"""CacheDir support
"""

import atexit
import json
import os
import shutil
import stat
import sys
import tempfile
import uuid

import SCons.Action
import SCons.Errors
import SCons.Warnings
import SCons.Util

CACHE_PREFIX_LEN = 2  # first two characters used as subdirectory name
CACHE_TAG = (
    b"Signature: 8a477f597d28d172789f06886806bc55\n"
    b"# SCons cache directory - see https://bford.info/cachedir/\n"
)

cache_enabled = True
cache_debug = False
cache_force = False
cache_show = False
cache_readonly = False
cache_tmp_uuid = uuid.uuid4().hex

def CacheRetrieveFunc(target, source, env) -> int:
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
            cd.copy_from_cache(env, cachefile, t.get_internal_path())
            try:
                os.utime(cachefile, None)
            except OSError:
                pass
        st = fs.stat(cachefile)
        fs.chmod(t.get_internal_path(), stat.S_IMODE(st.st_mode) | stat.S_IWRITE)
    return 0

def CacheRetrieveString(target, source, env) -> str:
    t = target[0]
    cd = env.get_CacheDir()
    cachedir, cachefile = cd.cachepath(t)
    if t.fs.exists(cachefile):
        return "Retrieved `%s' from cache" % t.get_internal_path()
    return ""

CacheRetrieve = SCons.Action.Action(CacheRetrieveFunc, CacheRetrieveString)

CacheRetrieveSilent = SCons.Action.Action(CacheRetrieveFunc, None)

def CachePushFunc(target, source, env) -> None:
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

    tempfile = "%s.tmp%s"%(cachefile,cache_tmp_uuid)
    errfmt = "Unable to copy %s to cache. Cache file is %s"

    try:
        fs.makedirs(cachedir, exist_ok=True)
    except OSError:
        msg = errfmt % (str(target), cachefile)
        raise SCons.Errors.SConsEnvironmentError(msg)
    try:
        if fs.islink(t.get_internal_path()):
            fs.symlink(fs.readlink(t.get_internal_path()), tempfile)
        else:
            cd.copy_to_cache(env, t.get_internal_path(), tempfile)
        fs.rename(tempfile, cachefile)

    except OSError:
        # It's possible someone else tried writing the file at the
        # same time we did, or else that there was some problem like
        # the CacheDir being on a separate file system that's full.
        # In any case, inability to push a file to cache doesn't affect
        # the correctness of the build, so just print a warning.
        msg = errfmt % (str(t), cachefile)
        cd.CacheDebug(errfmt + '\n', str(t), cachefile)
        SCons.Warnings.warn(SCons.Warnings.CacheWriteErrorWarning, msg)

CachePush = SCons.Action.Action(CachePushFunc, None)


class CacheDir:

    def __init__(self, path) -> None:
        """Initialize a CacheDir object.

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
        self.config = {}
        if path is not None:
            self._readconfig(path)

    def _add_config(self, path: str) -> None:
        """Create the cache config file in *path*.

        Locking isn't necessary in the normal case - when the cachedir is
        being created - because it's written to a unique directory first,
        before the directory is renamed. But it is legal to call CacheDir
        with an existing directory, which may be missing the config file,
        and in that case we do need locking. Simpler to always lock.
        """
        config_file = os.path.join(path, 'config')
        # TODO: this breaks the "unserializable config object" test which
        #   does some crazy stuff, so for now don't use setdefault. It does
        #   seem like it would be better to preserve an exisiting value.
        # self.config.setdefault('prefix_len', CACHE_PREFIX_LEN)
        self.config['prefix_len'] = CACHE_PREFIX_LEN
        with SCons.Util.FileLock(config_file, timeout=5, writer=True), open(
            config_file, "x"
        ) as config:
            try:
                json.dump(self.config, config)
            except Exception:
                msg = "Failed to write cache configuration for " + path
                raise SCons.Errors.SConsEnvironmentError(msg)

        # Add the tag file "carelessly" - the contents are not used by SCons
        # so we don't care about the chance of concurrent writes.
        try:
            tagfile = os.path.join(path, "CACHEDIR.TAG")
            with open(tagfile, 'xb') as cachedir_tag:
                cachedir_tag.write(CACHE_TAG)
        except FileExistsError:
            pass

    def _mkdir_atomic(self, path: str) -> bool:
        """Create cache directory at *path*.

        Uses directory renaming to avoid races.  If we are actually
        creating the dir, populate it with the metadata files at the
        same time as that's the safest way. But it's not illegal to point
        CacheDir at an existing directory that wasn't a cache previously,
        so we may have to do that elsewhere, too.

        Returns:
            ``True`` if it we created the dir, ``False`` if already existed,

        Raises:
            SConsEnvironmentError: if we tried and failed to create the cache.
        """
        directory = os.path.abspath(path)
        if os.path.exists(directory):
            return False

        try:
            # TODO: Python 3.7. See comment below.
            # tempdir = tempfile.TemporaryDirectory(dir=os.path.dirname(directory))
            tempdir = tempfile.mkdtemp(dir=os.path.dirname(directory))
        except OSError as e:
            msg = "Failed to create cache directory " + path
            raise SCons.Errors.SConsEnvironmentError(msg) from e

        # TODO: Python 3.7: the context manager raises exception on cleanup
        #    if the temporary was moved successfully (File Not Found).
        #    Fixed in 3.8+. In the replacement below we manually clean up if
        #    the move failed as mkdtemp() does not. TemporaryDirectory's
        #    cleanup is more sophisitcated so prefer when we can use it.
        # self._add_config(tempdir.name)
        # with tempdir:
        #     try:
        #         os.replace(tempdir.name, directory)
        #         return True
        #     except OSError as e:
        #         # did someone else get there first?
        #         if os.path.isdir(directory):
        #             return False  # context manager cleans up
        #         msg = "Failed to create cache directory " + path
        #         raise SCons.Errors.SConsEnvironmentError(msg) from e

        self._add_config(tempdir)
        try:
            os.replace(tempdir, directory)
            return True
        except OSError as e:
            # did someone else get there first? attempt cleanup.
            if os.path.isdir(directory):
                try:
                    shutil.rmtree(tempdir)
                except Exception:  # we tried, don't worry about it
                    pass
                return False
            msg = "Failed to create cache directory " + path
            raise SCons.Errors.SConsEnvironmentError(msg) from e

    def _readconfig(self, path: str) -> None:
        """Read the cache config from *path*.

        If directory or config file do not exist, create and populate.
        """
        config_file = os.path.join(path, 'config')
        created = self._mkdir_atomic(path)
        if not created and not os.path.isfile(config_file):
            # Could have been passed an empty directory
            self._add_config(path)
        try:
            with SCons.Util.FileLock(config_file, timeout=5, writer=False), open(
                config_file
            ) as config:
                self.config = json.load(config)
        except (ValueError, json.decoder.JSONDecodeError):
            msg = "Failed to read cache configuration for " + path
            raise SCons.Errors.SConsEnvironmentError(msg)

    def CacheDebug(self, fmt, target, cachefile) -> None:
        if cache_debug != self.current_cache_debug:
            if cache_debug == '-':
                self.debugFP = sys.stdout
            elif cache_debug:
                def debug_cleanup(debugFP) -> None:
                    debugFP.close()

                self.debugFP = open(cache_debug, 'w')
                atexit.register(debug_cleanup, self.debugFP)
            else:
                self.debugFP = None
            self.current_cache_debug = cache_debug
        if self.debugFP:
            self.debugFP.write(fmt % (target, os.path.split(cachefile)[1]))
            self.debugFP.write("requests: %d, hits: %d, misses: %d, hit rate: %.2f%%\n" %
                               (self.requests, self.hits, self.misses, self.hit_ratio))

    @classmethod
    def copy_from_cache(cls, env, src, dst) -> str:
        """Copy a file from cache."""
        if env.cache_timestamp_newer:
            return env.fs.copy(src, dst)
        else:
            return env.fs.copy2(src, dst)

    @classmethod
    def copy_to_cache(cls, env, src, dst) -> str:
        """Copy a file to cache.

        Just use the FS copy2 ("with metadata") method, except do an additional
        check and if necessary a chmod to ensure the cachefile is writeable,
        to forestall permission problems if the cache entry is later updated.
        """
        try:
            result = env.fs.copy2(src, dst)
            st = stat.S_IMODE(os.stat(result).st_mode)
            if not st | stat.S_IWRITE:
                os.chmod(dst, st | stat.S_IWRITE)
            return result
        except AttributeError as ex:
            raise OSError from ex

    @property
    def hit_ratio(self) -> float:
        return (100.0 * self.hits / self.requests if self.requests > 0 else 100)

    @property
    def misses(self) -> int:
        return self.requests - self.hits

    def is_enabled(self) -> bool:
        return cache_enabled and self.path is not None

    def is_readonly(self) -> bool:
        return cache_readonly

    def get_cachedir_csig(self, node) -> str:
        cachedir, cachefile = self.cachepath(node)
        if cachefile and os.path.exists(cachefile):
            return SCons.Util.hash_file_signature(cachefile, SCons.Node.FS.File.hash_chunksize)

    def cachepath(self, node) -> tuple:
        """Return where to cache a file.

        Given a Node, obtain the configured cache directory and
        the path to the cached file, which is generated from the
        node's build signature. If caching is not enabled for the
        None, return a tuple of None.
        """
        if not self.is_enabled():
            return None, None

        sig = node.get_cachedir_bsig()
        subdir = sig[:self.config['prefix_len']].upper()
        cachedir = os.path.join(self.path, subdir)
        return cachedir, os.path.join(cachedir, sig)

    def retrieve(self, node) -> bool:
        """Retrieve a node from cache.

        Returns True if a successful retrieval resulted.

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
