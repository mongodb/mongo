#!/usr/bin/python

'''Script to attempt an isolated build of the C++ driver and its examples.

Working directory must be the repository root.

Usage:

./buildscripts/build_and_test_client.py <mongo client archive file> [optional scons arguments]

The client is built in a temporary directory, and the sample programs are run against a mongod
instance found in the current working directory.  The temporary directory and its contents are
destroyed at the end of execution.
'''

import os
import shutil
import subprocess
import sys
import tempfile
import tarfile
import zipfile

import utils

def main(args):
    archive_file = args[1]
    scons_args = args[2:]
    build_and_test(archive_file, scons_args)

def build_and_test(archive_name, scons_args):
    work_dir = tempfile.mkdtemp()
    try:
        archive = open_archive(archive_name)
        extracted_root = extract_archive(work_dir, archive)
        run_scons(extracted_root, scons_args)
        smoke_client(extracted_root)
    finally:
        shutil.rmtree(work_dir)

def open_tar(archive_name):
    return tarfile.open(archive_name, 'r')

def open_zip(archive_name):
    class ZipWrapper(zipfile.ZipFile):
        def getnames(self):
            return self.namelist()
    return ZipWrapper(archive_name, 'r')

def open_archive(archive_name):
    try:
        return open_tar(archive_name)
    except:
        return open_zip(archive_name)

def extract_archive(work_dir, archive_file):
    archive_file.extractall(path=work_dir)
    return os.path.join(
        work_dir,
        os.path.dirname([n for n in archive_file.getnames() if n.endswith('SConstruct')][0])
        )

def run_scons(extracted_root, scons_args):
    rc = subprocess.call(['scons', '-C', extracted_root, ] + scons_args + ['clientTests'])
    if rc is not 0:
        sys.exit(rc)

def smoke_client(extracted_root):
    rc = subprocess.call(utils.smoke_command("--test-path", extracted_root, "client"))
    if rc is not 0:
        sys.exit(rc)

if __name__ == '__main__':
    main(sys.argv)
    sys.exit(0)
