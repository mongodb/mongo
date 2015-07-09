# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# ToCAsciiArray and ToCArray are from V8's js2c.py.
#
# Copyright 2012 the V8 project authors. All rights reserved.
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
#       copyright notice, this list of conditions and the following
#       disclaimer in the documentation and/or other materials provided
#       with the distribution.
#     * Neither the name of Google Inc. nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# This utility converts JS files containing self-hosted builtins into a C
# header file that can be embedded into SpiderMonkey.
#
# It uses the C preprocessor to process its inputs.

from __future__ import with_statement
import re, sys, os, fileinput, subprocess
import shlex
import which
from optparse import OptionParser

def ToCAsciiArray(lines):
  result = []
  for chr in lines:
    value = ord(chr)
    assert value < 128
    result.append(str(value))
  return ", ".join(result)

def ToCArray(lines):
  result = []
  for chr in lines:
    result.append(str(ord(chr)))
  return ", ".join(result)

HEADER_TEMPLATE = """\
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

namespace js {
namespace selfhosted {
    static const %(sources_type)s data[] = { %(sources_data)s };

    static const %(sources_type)s * const %(sources_name)s = reinterpret_cast<const %(sources_type)s *>(data);

    uint32_t GetCompressedSize() {
        return %(compressed_total_length)i;
    }

    uint32_t GetRawScriptsSize() {
        return %(raw_total_length)i;
    }
} // selfhosted
} // js
"""

def embed(cxx, preprocessorOption, msgs, sources, c_out, js_out, env):
  combinedSources = '\n'.join([msgs] + ['#include "%(s)s"' % { 's': source } for source in sources])
  args = ['-D%(k)s=%(v)s' % { 'k': k, 'v': env[k] } for k in env]
  preprocessed = preprocess(cxx, preprocessorOption, combinedSources, args)
  processed = '\n'.join([line for line in preprocessed.splitlines() if \
                         (line.strip() and not line.startswith('#'))])

  with open(js_out, 'w') as output:
    output.write(processed)
  with open(c_out, 'w') as output:
    import zlib
    compressed = zlib.compress(processed)
    data = ToCArray(compressed)
    output.write(HEADER_TEMPLATE % {
        'sources_type': 'unsigned char',
        'sources_data': data,
        'sources_name': 'compressedSources',
        'compressed_total_length': len(compressed),
        'raw_total_length': len(processed)
    })

def preprocess(cxx, preprocessorOption, source, args = []):
  if (not os.path.exists(cxx[0])):
    cxx[0] = which.which(cxx[0])
  # Clang seems to complain and not output anything if the extension of the
  # input is not something it recognizes, so just fake a .cpp here.
  tmpIn = 'self-hosting-cpp-input.cpp';
  tmpOut = 'self-hosting-preprocessed.pp';
  outputArg = shlex.split(preprocessorOption + tmpOut)

  with open(tmpIn, 'wb') as input:
    input.write(source)
  print(' '.join(cxx + outputArg + args + [tmpIn]))
  result = subprocess.Popen(cxx + outputArg + args + [tmpIn]).wait()
  if (result != 0):
    sys.exit(result);
  with open(tmpOut, 'r') as output:
    processed = output.read();
  os.remove(tmpIn)
  os.remove(tmpOut)
  return processed

def messages(jsmsg):
  defines = []
  for line in open(jsmsg):
    match = re.match("MSG_DEF\((JSMSG_(\w+))", line)
    if match:
      defines.append("#define %s %i" % (match.group(1), len(defines)))
    else:
      # Make sure that MSG_DEF isn't preceded by whitespace
      assert not line.strip().startswith("MSG_DEF")
  return '\n'.join(defines)

def main():
  env = {}
  def define_env(option, opt, value, parser):
    pair = value.split('=', 1)
    if len(pair) == 1:
      pair.append(1)
    env[pair[0]] = pair[1]
  p = OptionParser(usage="%prog [options] file")
  p.add_option('-D', action='callback', callback=define_env, type="string",
               metavar='var=[val]', help='Define a variable')
  p.add_option('-m', type='string', metavar='jsmsg', default='../js.msg',
               help='js.msg file')
  p.add_option('-c', type='string', metavar='cxx', help='Path to C++ compiler')
  p.add_option('-p', type='string', dest='p', metavar='cxxoption',
               help='Argument to compiler for preprocessing into an output file')
  p.add_option('-o', type='string', metavar='filename', default='selfhosted.out.h',
               help='C array header file')
  p.add_option('-s', type='string', metavar='jsfilename', default='selfhosted.js',
               help='Combined postprocessed JS file')
  (options, sources) = p.parse_args()
  if not (options.p and sources):
    p.print_help()
    sys.exit(1)
  cxx = shlex.split(options.c)
  msgs = messages(options.m)
  embed(cxx, options.p, msgs, sources, options.o, options.s, env)

if __name__ == "__main__":
  main()
