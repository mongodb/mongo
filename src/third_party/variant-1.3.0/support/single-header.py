#! /usr/bin/env python

# MPark.Variant
#
# Copyright Michael Park, 2017
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

import os.path
import re
import subprocess
import sys

# Prints a single header version of `include/mpark/variant.hpp` to stdout.

processed = []

def process(header):
  result = ''
  with open(header, 'r') as f:
    for line in f:
      p = re.compile('^#include "(.+)"')
      m = p.match(line)
      if m is None:
        result += line
      else:
        g = m.group(1)
        include = os.path.normpath(os.path.join(os.path.dirname(header), g))
        if include not in processed:
          result += process(include)
          result += '\n'
          processed.append(include)
  return result

root = subprocess.check_output(['git', 'rev-parse', '--show-toplevel']).strip()
result = process(os.path.join(root, 'include/mpark/variant.hpp'))

sys.stdout.write(result)
