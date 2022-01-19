# Copyright 2021 MongoDB Inc.
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

import datetime
import json
import os
import sys
import pathlib
import shutil
import tempfile
import traceback


import SCons

cache_debug_suffix = " (target: %s, cachefile: %s) "

class InvalidChecksum(SCons.Errors.BuildError):
    def __init__(self, src, dst, reason, cache_csig='', computed_csig=''):
        self.message = f"ERROR: md5 checksum {reason} for {src} ({dst})"
        self.cache_csig = cache_csig
        self.computed_csig = computed_csig
    def __str__(self):
        return self.message

class CacheTransferFailed(SCons.Errors.BuildError):
    def __init__(self, src, dst, reason):
        self.message = f"ERROR: cachedir transfer {reason} while transfering {src} to {dst}"

    def __str__(self):
        return self.message

class UnsupportedError(SCons.Errors.BuildError):
    def __init__(self, class_name, feature):
        self.message = f"{class_name} does not support {feature}"

    def __str__(self):
        return self.message

class CacheDirValidate(SCons.CacheDir.CacheDir):

    def __init__(self, path):
        self.json_log = None
        super().__init__(path)

    @staticmethod
    def get_ext():
        # Cache prune script is allowing only directories with this extension
        # if this is changed, cache prune script should also be updated.
        return '.cksum'

    @staticmethod
    def get_file_contents_path(default_cachefile_path):
        return pathlib.Path(default_cachefile_path) / pathlib.Path(default_cachefile_path).name.split('.')[0]

    @staticmethod
    def get_bad_cachefile_path(cksum_cachefile_dir):
        return pathlib.Path(cksum_cachefile_dir) / 'bad_cache_file'

    @staticmethod
    def get_hash_path(cksum_cachefile_path):
        return pathlib.Path(cksum_cachefile_path).parent / 'content_hash'

    @staticmethod
    def get_cachedir_path(path):
        return str(pathlib.Path(path + CacheDirValidate.get_ext()))

    @classmethod
    def copy_from_cache(cls, env, src, dst):

        if not str(pathlib.Path(src)).endswith(cls.get_ext()):
            return super().copy_from_cache(env, src, dst)

        if env.cache_timestamp_newer:
            raise UnsupportedError(cls.__name__, "timestamp-newer")

        src_file = cls.get_file_contents_path(src)
        # using os.path.exists here because: https://bugs.python.org/issue35306
        if os.path.exists(str(cls.get_bad_cachefile_path(src))):
            raise InvalidChecksum(cls.get_hash_path(src_file), dst, f"cachefile marked as bad checksum")

        csig = None
        try:
            with open(cls.get_hash_path(src_file), 'rb') as f_out:
                csig = f_out.read().decode().strip()
        except OSError as ex:
            raise InvalidChecksum(cls.get_hash_path(src_file), dst, f"failed to read hash file: {ex}") from ex
        else:
            if not csig:
                raise InvalidChecksum(cls.get_hash_path(src_file), dst, f"no content_hash data found")

        with tempfile.TemporaryDirectory() as tmpdirname:
            dst_tmp = pathlib.Path(tmpdirname) / os.path.basename(dst)
            try:
                shutil.copy2(src_file, dst_tmp)
            except OSError as ex:
                raise CacheTransferFailed(src_file, dst, f"failed to copy from cache: {ex}") from ex
            else:
                shutil.move(dst_tmp, dst)

        new_csig = SCons.Util.MD5filesignature(dst,
            chunksize=SCons.Node.FS.File.md5_chunksize*1024)

        if csig != new_csig:
            raise InvalidChecksum(
                cls.get_hash_path(src_file), dst, f"checksums don't match {csig} != {new_csig}", cache_csig=csig, computed_csig=new_csig)

    @classmethod
    def copy_to_cache(cls, env, src, dst):

        # dst is bsig/file from cachepath method, so
        # we make sure to make the bsig dir first
        dst = pathlib.Path(dst)
        dst_file = dst / dst.name.split('.')[0]

        try:
            os.makedirs(dst, exist_ok=True)
            super().copy_to_cache(env, src, dst_file)
        except OSError as ex:
            raise CacheTransferFailed(src, dst_file, f"failed to copy to cache: {ex}") from ex

        try:
            with open(cls.get_hash_path(dst_file), 'w') as f_out:
                f_out.write(env.File(src).get_content_hash())
        except OSError as ex:
            raise CacheTransferFailed(src, dst_file, f"failed to create hash file: {ex}") from ex

    def log_json_cachedebug(self, node, pushing=False):
        if (pushing
            and (node.nocache or SCons.CacheDir.cache_readonly or 'conftest' in str(node))):
                return

        cachefile = self.get_file_contents_path(self.cachepath(node)[1])
        if node.fs.exists(cachefile):
            cache_event = 'double_push' if pushing else 'hit'
        else:
            cache_event = 'push' if pushing else 'miss'

        self.CacheDebugJson({'type': cache_event}, node, cachefile)

    def retrieve(self, node):
        if not self.is_enabled():
            return False

        self.log_json_cachedebug(node)
        try:
            return super().retrieve(node)
        except InvalidChecksum as ex:
            self.print_cache_issue(node, ex)
            self.clean_bad_cachefile(node, ex.cache_csig, ex.computed_csig)
            return False
        except (UnsupportedError, CacheTransferFailed) as ex:
            self.print_cache_issue(node, ex)
            return False

    def push(self, node):
        if self.is_readonly() or not self.is_enabled():
            return
        self.log_json_cachedebug(node, pushing=True)
        try:
            return super().push(node)
        except CacheTransferFailed as ex:
            self.print_cache_issue(node, ex)
            return False

    def CacheDebugJson(self, json_data, target, cachefile):

        if SCons.CacheDir.cache_debug and SCons.CacheDir.cache_debug != '-' and self.json_log is None:
            self.json_log = open(SCons.CacheDir.cache_debug + '.json', 'w')

        if self.json_log is not None:
            cksum_cachefile = str(pathlib.Path(cachefile).parent)
            if cksum_cachefile.endswith(self.get_ext()):
                cachefile = cksum_cachefile

            json_data.update({
                'timestamp': str(datetime.datetime.now(datetime.timezone.utc)),
                'realfile': str(target),
                'cachefile': pathlib.Path(cachefile).name,
                'cache_dir': str(pathlib.Path(cachefile).parent.parent),
            })

            # capture exception information
            if sys.exc_info()[1]:
                json_data.update({'error': self._format_exception_msg()})

            self.json_log.write(json.dumps(json_data) + '\n')

    def CacheDebug(self, fmt, target, cachefile):

        super().CacheDebug(fmt, target, cachefile)
        # Capture exception information into the cache debug log
        if sys.exc_info()[1] and self.debugFP:
            self.debugFP.write(self._format_exception_msg())

    def _format_exception_msg(self):
        return ('An exception was detected while using the cache:\n' +
                '    ' + "\n    ".join("".join(traceback.format_exc()).split("\n"))) + '\n'

    def _log(self, log_msg, json_info, realnode, cachefile):
        self.CacheDebug(log_msg + cache_debug_suffix, realnode, cachefile)
        self.CacheDebugJson(json_info, realnode, cachefile)

    def print_cache_issue(self, node, ex):

        cksum_dir = pathlib.Path(self.cachepath(node)[1])
        self._log(str(ex), {'type': 'error'}, node, cksum_dir)

    def clean_bad_cachefile(self, node, cache_csig, computed_csig):

        cksum_dir = pathlib.Path(self.cachepath(node)[1])
        rm_path = f"{cksum_dir}.{SCons.CacheDir.cache_tmp_uuid}.del"
        try:
            try:
                pathlib.Path(self.get_bad_cachefile_path(cksum_dir)).touch()
            except FileExistsError:
                pass
            cksum_dir.replace(rm_path)
        except OSError as ex:
            msg = f"Failed to rename {cksum_dir} to {rm_path}: {ex}"
            self._log(msg, {'type': 'error'}, node, cksum_dir)
            return

        msg = f"Removed bad cachefile {cksum_dir} found in cache."
        self._log(msg, {
            'type': 'invalid_checksum',
            'cache_csig': cache_csig,
            'computed_csig': computed_csig
        }, node, cksum_dir)


    def get_cachedir_csig(self, node):
        cachedir, cachefile = self.cachepath(node)
        if cachefile and os.path.exists(cachefile):
            with open(self.get_hash_path(self.get_file_contents_path(cachefile)), 'rb') as f_out:
                return f_out.read().decode()

    def cachepath(self, node):
        if not self.is_enabled():
            return None, None

        dir, path = super().cachepath(node)
        if node.fs.exists(path):
            return dir, path
        return dir, str(self.get_cachedir_path(path))

def exists(env):
    return True

def generate(env):
    if not env.get('CACHEDIR_CLASS'):
        env['CACHEDIR_CLASS'] = CacheDirValidate
