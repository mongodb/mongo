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

import os
import pathlib
import shutil

import SCons

cache_debug_suffix = " (target: %s, cachefile: %s) "

class InvalidChecksum(SCons.Errors.BuildError):
    def __init__(self, src, dst, reason):
        self.message = f"ERROR: md5 checksum {reason} for {src} ({dst})"

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

    @staticmethod
    def get_ext():
        # Cache prune script is allowing only directories with this extension
        # if this is changed, cache prune script should also be updated.
        return '.cksum'

    @staticmethod
    def get_file_contents_path(path):
        return str(pathlib.Path(path + CacheDirValidate.get_ext()) / pathlib.Path(path).name)

    @staticmethod
    def get_hash_path(path):
        return str(pathlib.Path(path).parent / 'content_hash')

    @classmethod
    def copy_from_cache(cls, env, src, dst):

        if not str(pathlib.Path(src).parent).endswith(cls.get_ext()):
            return super().copy_from_cache(env, src, dst)

        if env.cache_timestamp_newer:
            raise UnsupportedError(cls.__name__, "timestamp-newer")

        csig = None
        invalid_cksum = InvalidChecksum(cls.get_hash_path(src), dst, "failed to read hash file")
        try:
            with open(cls.get_hash_path(src), 'rb') as f_out:
                csig = f_out.read().decode().strip()
        except OSError as ex:
            raise invalid_cksum from ex
        finally:
            if not csig:
                raise invalid_cksum from ex

        try:
            shutil.copy2(src, dst)
        except OSError as ex:
            raise CacheTransferFailed(src, dst, "failed to copy from cache") from ex

        new_csig = SCons.Util.MD5filesignature(dst,
            chunksize=SCons.Node.FS.File.md5_chunksize*1024)

        if csig != new_csig:
            raise InvalidChecksum(
                cls.get_hash_path(src), dst, f"checksums don't match {csig} != {new_csig}")

    @classmethod
    def copy_to_cache(cls, env, src, dst):

        # dst is bsig/file from cachepath method, so
        # we make sure to make the bsig dir first
        os.makedirs(pathlib.Path(dst).parent, exist_ok=True)

        try:
            shutil.copy2(src, dst)
        except OSError as ex:
            raise CacheTransferFailed(src, dst, "failed to copy to cache") from ex

        try:
            with open(cls.get_hash_path(dst), 'w') as f_out:
                f_out.write(env.File(src).get_content_hash())
        except OSError as ex:
            raise CacheTransferFailed(src, dst, "failed to create hash file") from ex

    def retrieve(self, node):
        try:
            return super().retrieve(node)
        except InvalidChecksum as ex:
            self.print_cache_issue(node, str(ex))
            self.clean_bad_cachefile(node)
            return False
        except (UnsupportedError, CacheTransferFailed) as ex:
            self.print_cache_issue(node, str(ex))
            return False

    def push(self, node):
        try:
            return super().push(node)
        except CacheTransferFailed as ex:
            self.print_cache_issue(node, str(ex))
            return False

    def print_cache_issue(self, node, msg):

        cksum_dir = pathlib.Path(self.cachepath(node)[1]).parent
        print(msg)
        self.CacheDebug(msg + cache_debug_suffix, node, cksum_dir)

    def clean_bad_cachefile(self, node):

        cksum_dir = pathlib.Path(self.cachepath(node)[1]).parent
        if cksum_dir.is_dir():
            rm_path = f"{cksum_dir}.{SCons.CacheDir.cache_tmp_uuid}.del"
            cksum_dir.replace(rm_path)
            shutil.rmtree(rm_path)

            clean_msg = f"Removed bad cachefile {cksum_dir} from cache."
            print(clean_msg)
            self.CacheDebug(clean_msg + cache_debug_suffix, node, cksum_dir)

    def get_cachedir_csig(self, node):
        cachedir, cachefile = self.cachepath(node)
        if cachefile and os.path.exists(cachefile):
            with open(self.get_hash_path(cachefile), 'rb') as f_out:
                return f_out.read().decode()

    def cachepath(self, node):
        dir, path = super().cachepath(node)
        if node.fs.exists(path):
            return dir, path
        return dir, self.get_file_contents_path(path)

def exists(env):
    return True

def generate(env):
    if not env.get('CACHEDIR_CLASS'):
        env['CACHEDIR_CLASS'] = CacheDirValidate
