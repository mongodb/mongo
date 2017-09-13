# Copyright (C) 2015 MongoDB Inc.
#
# This program is free software: you can redistribute it and/or  modify
# it under the terms of the GNU Affero General Public License, version 3,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import SCons
import os
import os.path as ospath
import subprocess
import shutil
import tarfile
import time
import zipfile
import StringIO

from distutils.spawn import find_executable

__distsrc_callbacks = []

class DistSrcFile:
    def __init__(self, **kwargs):
        [ setattr(self, key, val) for (key, val) in kwargs.items() ]

    def __str__(self):
        return self.name

class DistSrcArchive:
    def __init__(self, archive_type, archive_file, filename, mode):
        self.archive_type = archive_type
        self.archive_file = archive_file
        self.archive_name = filename
        self.archive_mode = mode

    @staticmethod
    def Open(filename):
        if filename.endswith("tar"):
            return DistSrcTarArchive(
                'tar',
                tarfile.open(filename, 'r', format=tarfile.PAX_FORMAT),
                filename,
                'r',
            )
        elif filename.endswith("zip"):
            return DistSrcZipArchive(
                'zip',
                zipfile.ZipFile(filename, 'a'),
                filename,
                'a',
            )

    def close(self):
        self.archive_file.close()

class DistSrcTarArchive(DistSrcArchive):
    def __iter__(self):
        file_list = self.archive_file.getnames()
        for name in file_list:
            yield name

    def __getitem__(self, key):
        item_data = self.archive_file.getmember(key)
        return DistSrcFile(
            name=key,
            size=item_data.size,
            mtime=item_data.mtime,
            mode=item_data.mode,
            type=item_data.type,
            uid=item_data.uid,
            gid=item_data.gid,
            uname=item_data.uname,
            gname=item_data.uname
        )

    def append_file_contents(self, filename, file_contents,
            mtime=time.time(),
            mode=0644,
            uname="root",
            gname="root"):
        file_metadata = tarfile.TarInfo(name=filename)
        file_metadata.mtime = mtime
        file_metadata.mode = mode
        file_metadata.uname = uname
        file_metadata.gname = gname
        file_metadata.size = len(file_contents)
        file_buf = StringIO.StringIO(file_contents)
        if self.archive_mode == 'r':
            self.archive_file.close()
            self.archive_file = tarfile.open(
                self.archive_name,
                'a',
                format=tarfile.PAX_FORMAT,
            )
            self.archive_mode = 'a'
        self.archive_file.addfile(file_metadata, fileobj=file_buf)

    def append_file(self, filename, localfile):
        self.archive_file.add(localfile, arcname=filename)

class DistSrcZipArchive(DistSrcArchive):
    def __iter__(self):
        file_list = self.archive_file.namelist()
        for name in file_list:
            yield name

    def __getitem__(self, key):
        item_data = self.archive_file.getinfo(key)
        fixed_time = item_data.date_time + (0, 0, 0)
        is_dir = key.endswith("/")
        return DistSrcFile(
            name=key,
            size=item_data.file_size,
            mtime=time.mktime(fixed_time),
            mode=0775 if is_dir else 0664,
            type=tarfile.DIRTYPE if is_dir else tarfile.REGTYPE,
            uid=0,
            gid=0,
            uname="root",
            gname="root"
        )

    def append_file_contents(self, filename, file_contents,
            mtime=time.time(),
            mode=0644,
            uname="root",
            gname="root"):
        self.archive_file.writestr(filename, file_contents)

    def append_file(self, filename, localfile):
        self.archive_file.write(localfile, arcname=filename)

def build_error_action(msg):
    def error_stub(target=None, source=None, env=None):
        print msg
        env.Exit(1)
    return [ error_stub ]

def distsrc_action_generator(source, target, env, for_signature):
    # This is done in two stages because env.WhereIs doesn't seem to work
    # correctly on Windows, but we still want to be able to override the PATH
    # using the env.
    git_path = env.WhereIs("git")
    if not git_path:
        git_path = find_executable("git")

    if not git_path:
        return build_error_action("Could not find git - cannot create distsrc archive")

    def run_distsrc_callbacks(target=None, source=None, env=None):
        archive_wrapper = DistSrcArchive.Open(str(target[0]))
        for fn in __distsrc_callbacks:
            fn(env, archive_wrapper)
        archive_wrapper.close()

    target_ext = str(target[0])[-3:]
    if not target_ext in [ 'zip', 'tar' ]:
        print "Invalid file format for distsrc. Must be tar or zip file"
        env.Exit(1)

    git_cmd = "\"%s\" archive --format %s --output %s --prefix ${MONGO_DIST_SRC_PREFIX} HEAD" % (
        git_path, target_ext, target[0])

    return [
        SCons.Action.Action(git_cmd, "Running git archive for $TARGET"),
        SCons.Action.Action(run_distsrc_callbacks, "Running distsrc callbacks for $TARGET")
    ]

def add_callback(env, fn):
    __distsrc_callbacks.append(fn)

def generate(env, **kwargs):
    env.AddMethod(add_callback, 'AddDistSrcCallback')
    env['BUILDERS']['__DISTSRC'] = SCons.Builder.Builder(
        generator=distsrc_action_generator,
    )

    def DistSrc(env, target):
        result = env.__DISTSRC(target=target, source=[])
        env.AlwaysBuild(result)
        env.NoCache(result)
        return result

    env.AddMethod(DistSrc, 'DistSrc')

def exists(env):
    return True
