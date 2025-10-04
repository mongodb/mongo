#!/usr/bin/env python3
"""Test zstd interoperability between versions"""

# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# You may select, at your option, one of the above-listed licenses.
# ################################################################

import filecmp
import glob
import hashlib
import os
import shutil
import sys
import subprocess
from subprocess import Popen, PIPE

repo_url = 'https://github.com/facebook/zstd.git'
tmp_dir_name = 'tests/versionsTest'
make_cmd = 'make'
make_args = ['-j','CFLAGS=-O0']
git_cmd = 'git'
test_dat_src = 'README.md'
test_dat = 'test_dat'
head = 'vdevel'
dict_source = 'dict_source'
dict_globs = [
    'programs/*.c',
    'lib/common/*.c',
    'lib/compress/*.c',
    'lib/decompress/*.c',
    'lib/dictBuilder/*.c',
    'lib/legacy/*.c',
    'programs/*.h',
    'lib/common/*.h',
    'lib/compress/*.h',
    'lib/dictBuilder/*.h',
    'lib/legacy/*.h'
]


def execute(command, print_output=False, print_error=True, param_shell=False):
    popen = Popen(command, stdout=PIPE, stderr=PIPE, shell=param_shell)
    stdout_lines, stderr_lines = popen.communicate()
    stderr_lines = stderr_lines.decode("utf-8")
    stdout_lines = stdout_lines.decode("utf-8")
    if print_output:
        print(stdout_lines)
        print(stderr_lines)
    if popen.returncode is not None and popen.returncode != 0:
        if not print_output and print_error:
            print(stderr_lines)
    return popen.returncode


def proc(cmd_args, pipe=True, dummy=False):
    if dummy:
        return
    if pipe:
        subproc = Popen(cmd_args, stdout=PIPE, stderr=PIPE)
    else:
        subproc = Popen(cmd_args)
    return subproc.communicate()


def make(targets, pipe=True):
    cmd = [make_cmd] + make_args + targets
    cmd_str = str(cmd)
    print('compilation command : ' + cmd_str)
    return proc(cmd, pipe)


def git(args, pipe=True):
    return proc([git_cmd] + args, pipe)


def get_git_tags():
    stdout, stderr = git(['tag', '-l', 'v[0-9].[0-9].[0-9]'])
    tags = stdout.decode('utf-8').split()
    return tags


def dict_ok(tag, dict_name, sample):
    if not os.path.isfile(dict_name):
        return False
    try:
        cmd = ['./zstd.' + tag, '-D', dict_name]
        with open(sample, "rb") as i:
            subprocess.check_call(cmd, stdin=i, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except:
        return False


def create_dict(tag, dict_source_path, fallback_tag=None):
    dict_name = 'dict.' + tag
    if not os.path.isfile(dict_name):
        cFiles = glob.glob(dict_source_path + "/*.c")
        hFiles = glob.glob(dict_source_path + "/*.h")
        # Ensure the dictionary builder is deterministic
        files = sorted(cFiles + hFiles)
        if tag == 'v0.5.0':
            result = execute('./dictBuilder.' + tag + ' ' + ' '.join(files) + ' -o ' + dict_name, print_output=False, param_shell=True)
        else:
            result = execute('./zstd.' + tag + ' -f --train ' + ' '.join(files) + ' -o ' + dict_name, print_output=False, param_shell=True)
        if result == 0 and dict_ok(tag, dict_name, files[0]):
            print(dict_name + ' created')
        elif fallback_tag is not None:
            fallback_dict_name = 'dict.' + fallback_tag
            print('creating dictionary ' + dict_name + ' failed, falling back to ' + fallback_dict_name)
            shutil.copy(fallback_dict_name, dict_name)
        else:
            raise RuntimeError('ERROR: creating of ' + dict_name + ' failed')
    else:
        print(dict_name + ' already exists')


def zstd(tag, args, input_file, output_file):
    """
    Zstd compress input_file to output_file.
    Need this helper because 0.5.0 is broken when stdout is not a TTY.
    Throws an exception if the command returns non-zero.
    """
    with open(input_file, "rb") as i:
        with open(output_file, "wb") as o:
            cmd = ['./zstd.' + tag] + args
            print("Running: '{}', input={}, output={}" .format(
                ' '.join(cmd), input_file, output_file
            ))
            result = subprocess.run(cmd, stdin=i, stdout=o, stderr=subprocess.PIPE)
            print("Stderr: {}".format(result.stderr.decode("ascii")))
            result.check_returncode()


def dict_compress_sample(tag, sample):
    dict_name = 'dict.' + tag
    verbose = ['-v', '-v', '-v']
    zstd(tag, ['-D', dict_name, '-1'] + verbose, sample, sample + '_01_64_' + tag + '_dictio.zst')
    zstd(tag, ['-D', dict_name, '-3'], sample, sample + '_03_64_' + tag + '_dictio.zst')
    zstd(tag, ['-D', dict_name, '-5'], sample, sample + '_05_64_' + tag + '_dictio.zst')
    zstd(tag, ['-D', dict_name, '-9'], sample, sample + '_09_64_' + tag + '_dictio.zst')
    zstd(tag, ['-D', dict_name, '-15'], sample, sample + '_15_64_' + tag + '_dictio.zst')
    zstd(tag, ['-D', dict_name, '-18'], sample, sample + '_18_64_' + tag + '_dictio.zst')
    # zstdFiles = glob.glob("*.zst*")
    # print(zstdFiles)
    print(tag + " : dict compression completed")


def compress_sample(tag, sample):
    zstd(tag, ['-1'], sample, sample + '_01_64_' + tag + '_nodict.zst')
    zstd(tag, ['-3'], sample, sample + '_03_64_' + tag + '_nodict.zst')
    zstd(tag, ['-5'], sample, sample + '_05_64_' + tag + '_nodict.zst')
    zstd(tag, ['-9'], sample, sample + '_09_64_' + tag + '_nodict.zst')
    zstd(tag, ['-15'], sample, sample + '_15_64_' + tag + '_nodict.zst')
    zstd(tag, ['-18'], sample, sample + '_18_64_' + tag + '_nodict.zst')
    # zstdFiles = glob.glob("*.zst*")
    # print(zstdFiles)
    print(tag + " : compression completed")


# https://stackoverflow.com/a/19711609/2132223
def sha1_of_file(filepath):
    with open(filepath, 'rb') as f:
        return hashlib.sha1(f.read()).hexdigest()


def remove_duplicates():
    list_of_zst = sorted(glob.glob('*.zst'))
    for i, ref_zst in enumerate(list_of_zst):
        if not os.path.isfile(ref_zst):
            continue
        for j in range(i + 1, len(list_of_zst)):
            compared_zst = list_of_zst[j]
            if not os.path.isfile(compared_zst):
                continue
            if filecmp.cmp(ref_zst, compared_zst):
                os.remove(compared_zst)
                print('duplicated : {} == {}'.format(ref_zst, compared_zst))


def decompress_zst(tag):
    dec_error = 0
    list_zst = sorted(glob.glob('*_nodict.zst'))
    for file_zst in list_zst:
        print(file_zst + ' ' + tag)
        file_dec = file_zst + '_d64_' + tag + '.dec'
        zstd(tag, ['-d'], file_zst, file_dec)
        if not filecmp.cmp(file_dec, test_dat):
            raise RuntimeError('Decompression failed: tag={} file={}'.format(tag, file_zst))
        else:
            print('OK     ')


def decompress_dict(tag):
    dec_error = 0
    list_zst = sorted(glob.glob('*_dictio.zst'))
    for file_zst in list_zst:
        dict_tag = file_zst[0:len(file_zst)-11]  # remove "_dictio.zst"
        if head in dict_tag: # find vdevel
            dict_tag = head
        else:
            dict_tag = dict_tag[dict_tag.rfind('v'):]
        if tag == 'v0.6.0' and dict_tag < 'v0.6.0':
            continue
        dict_name = 'dict.' + dict_tag
        print(file_zst + ' ' + tag + ' dict=' + dict_tag)
        file_dec = file_zst + '_d64_' + tag + '.dec'
        zstd(tag, ['-D', dict_name, '-d'], file_zst, file_dec)
        if not filecmp.cmp(file_dec, test_dat):
            raise RuntimeError('Decompression failed: tag={} file={}'.format(tag, file_zst))
        else:
            print('OK     ')


if __name__ == '__main__':
    error_code = 0
    base_dir = os.getcwd() + '/..'                  # /path/to/zstd
    tmp_dir = base_dir + '/' + tmp_dir_name         # /path/to/zstd/tests/versionsTest
    clone_dir = tmp_dir + '/' + 'zstd'              # /path/to/zstd/tests/versionsTest/zstd
    dict_source_path = tmp_dir + '/' + dict_source  # /path/to/zstd/tests/versionsTest/dict_source
    programs_dir = base_dir + '/programs'           # /path/to/zstd/programs
    os.makedirs(tmp_dir, exist_ok=True)

    # since Travis clones limited depth, we should clone full repository
    if not os.path.isdir(clone_dir):
        git(['clone', repo_url, clone_dir])

    shutil.copy2(base_dir + '/' + test_dat_src, tmp_dir + '/' + test_dat)

    # Retrieve all release tags
    print('Retrieve all release tags :')
    os.chdir(clone_dir)
    alltags = get_git_tags() + [head]
    tags = [t for t in alltags if t >= 'v0.5.0']
    print(tags)

    # Build all release zstd
    for tag in tags:
        os.chdir(base_dir)
        dst_zstd = '{}/zstd.{}'.format(tmp_dir, tag)  # /path/to/zstd/tests/versionsTest/zstd.<TAG>
        if not os.path.isfile(dst_zstd) or tag == head:
            if tag != head:
                print('-----------------------------------------------')
                print('compiling ' + tag)
                print('-----------------------------------------------')
                r_dir = '{}/{}'.format(tmp_dir, tag)  # /path/to/zstd/tests/versionsTest/<TAG>
                os.makedirs(r_dir, exist_ok=True)
                os.chdir(clone_dir)
                git(['--work-tree=' + r_dir, 'checkout', tag, '--', '.'], False)
                if tag == 'v0.5.0':
                    os.chdir(r_dir + '/dictBuilder')  # /path/to/zstd/tests/versionsTest/v0.5.0/dictBuilder
                    make(['clean'], False)   # separate 'clean' target to allow parallel build
                    make(['dictBuilder'], False)
                    shutil.copy2('dictBuilder', '{}/dictBuilder.{}'.format(tmp_dir, tag))
                os.chdir(r_dir + '/programs')  # /path/to/zstd/tests/versionsTest/<TAG>/programs
                make(['clean'], False)  # separate 'clean' target to allow parallel build
                make(['zstd'], False)
            else:
                os.chdir(programs_dir)
                print('-----------------------------------------------')
                print('compiling head')
                print('-----------------------------------------------')
                make(['zstd'], False)
            shutil.copy2('zstd', dst_zstd)

    # remove any remaining *.zst and *.dec from previous test
    os.chdir(tmp_dir)
    for compressed in glob.glob("*.zst"):
        os.remove(compressed)
    for dec in glob.glob("*.dec"):
        os.remove(dec)

    # copy *.c and *.h to a temporary directory ("dict_source")
    if not os.path.isdir(dict_source_path):
        os.mkdir(dict_source_path)
        for dict_glob in dict_globs:
            files = glob.glob(dict_glob, root_dir=base_dir)
            for file in files:
                file = os.path.join(base_dir, file)
                print("copying " + file + " to " + dict_source_path)
                shutil.copy(file, dict_source_path)

    print('-----------------------------------------------')
    print('Compress test.dat by all released zstd')
    print('-----------------------------------------------')

    create_dict(head, dict_source_path)
    for tag in tags:
        print(tag)
        if tag >= 'v0.5.0':
            create_dict(tag, dict_source_path, head)
            dict_compress_sample(tag, test_dat)
            remove_duplicates()
            decompress_dict(tag)
        compress_sample(tag, test_dat)
        remove_duplicates()
        decompress_zst(tag)

    print('')
    print('Enumerate different compressed files')
    zstds = sorted(glob.glob('*.zst'))
    for zstd in zstds:
        print(zstd + ' : ' + repr(os.path.getsize(zstd)) + ', ' + sha1_of_file(zstd))
