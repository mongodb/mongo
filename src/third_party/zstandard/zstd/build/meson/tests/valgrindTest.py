#!/usr/bin/env python3
# #############################################################################
# Copyright (c) 2018-present    lzutao <taolzu(at)gmail.com>
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# #############################################################################
import os
import subprocess
import tempfile


def valgrindTest(valgrind, datagen, fuzzer, zstd, fullbench):
  VALGRIND_ARGS = [valgrind, '--leak-check=full', '--show-leak-kinds=all', '--error-exitcode=1']

  print('\n ---- valgrind tests : memory analyzer ----')

  subprocess.check_call([*VALGRIND_ARGS, datagen, '-g50M'], stdout=subprocess.DEVNULL)

  if subprocess.call([*VALGRIND_ARGS, zstd],
                     stdout=subprocess.DEVNULL) == 0:
    raise subprocess.SubprocessError('zstd without argument should have failed')

  with subprocess.Popen([datagen, '-g80'], stdout=subprocess.PIPE) as p1, \
       subprocess.Popen([*VALGRIND_ARGS, zstd, '-', '-c'],
                        stdin=p1.stdout,
                        stdout=subprocess.DEVNULL) as p2:
    p1.stdout.close()  # Allow p1 to receive a SIGPIPE if p2 exits.
    p2.communicate()
    if p2.returncode != 0:
      raise subprocess.SubprocessError()

  with subprocess.Popen([datagen, '-g16KB'], stdout=subprocess.PIPE) as p1, \
       subprocess.Popen([*VALGRIND_ARGS, zstd, '-vf', '-', '-c'],
                        stdin=p1.stdout,
                        stdout=subprocess.DEVNULL) as p2:
    p1.stdout.close()
    p2.communicate()
    if p2.returncode != 0:
      raise subprocess.SubprocessError()

  with tempfile.NamedTemporaryFile() as tmp_fd:
    with subprocess.Popen([datagen, '-g2930KB'], stdout=subprocess.PIPE) as p1, \
         subprocess.Popen([*VALGRIND_ARGS, zstd, '-5', '-vf', '-', '-o', tmp_fd.name],
                          stdin=p1.stdout) as p2:
      p1.stdout.close()
      p2.communicate()
      if p2.returncode != 0:
        raise subprocess.SubprocessError()

    subprocess.check_call([*VALGRIND_ARGS, zstd, '-vdf', tmp_fd.name, '-c'],
                          stdout=subprocess.DEVNULL)

    with subprocess.Popen([datagen, '-g64MB'], stdout=subprocess.PIPE) as p1, \
         subprocess.Popen([*VALGRIND_ARGS, zstd, '-vf', '-', '-c'],
                          stdin=p1.stdout,
                          stdout=subprocess.DEVNULL) as p2:
      p1.stdout.close()
      p2.communicate()
      if p2.returncode != 0:
        raise subprocess.SubprocessError()

  subprocess.check_call([*VALGRIND_ARGS, fuzzer, '-T1mn', '-t1'])
  subprocess.check_call([*VALGRIND_ARGS, fullbench, '-i1'])


def main():
  import argparse
  parser = argparse.ArgumentParser(description='Valgrind tests : memory analyzer')
  parser.add_argument('valgrind', help='valgrind path')
  parser.add_argument('zstd', help='zstd path')
  parser.add_argument('datagen', help='datagen path')
  parser.add_argument('fuzzer', help='fuzzer path')
  parser.add_argument('fullbench', help='fullbench path')

  args = parser.parse_args()

  valgrind = args.valgrind
  zstd = args.zstd
  datagen = args.datagen
  fuzzer = args.fuzzer
  fullbench = args.fullbench

  valgrindTest(valgrind, datagen, fuzzer, zstd, fullbench)


if __name__ == '__main__':
  main()
