# MPark.Variant
#
# Copyright Michael Park, 2015-2017
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

import os
import pprint
import subprocess

result = {}

std_flags = os.getenv('STDFLAGS')
for std_flag in std_flags.split() if std_flags is not None else ['']:
  os.environ['CXXFLAGS'] = std_flag
  for exceptions in ['OFF', 'ON']:
    for build_type in ['Debug', 'Release']:
      config = '{}-{}-{}'.format(
          filter(str.isalnum, std_flag), exceptions, build_type)
      build_dir = 'build-{}'.format(config)
      os.mkdir(build_dir)
      os.chdir(build_dir)
      result[config] = { 'Configure': None, 'Build': None, 'Test': None }

      tests = os.environ['TESTS'].split()
      if std_flag.endswith(('11', '1y', '14', '1z')) and 'libc++' in tests:
        tests.remove('libc++')

      result[config]['Configure'] = subprocess.call([
        'cmake', '-GNinja',
                 '-DCMAKE_BUILD_TYPE={}'.format(build_type),
                 '-DMPARK_VARIANT_EXCEPTIONS={}'.format(exceptions),
                 '-DMPARK_VARIANT_INCLUDE_TESTS={}'.format(';'.join(tests)),
                 '..',
      ])
      if result[config]['Configure'] == 0:
        result[config]['Build'] = subprocess.call([
          'cmake', '--build', '.', '--', '-k', '0'])
        if result[config]['Build'] == 0:
          result[config]['Test'] = subprocess.call([
            'ctest', '--output-on-failure'])
      os.chdir('..')

pprint.pprint(result)
exit(any(status != 0 for d in result.itervalues() for status in d.itervalues()))
