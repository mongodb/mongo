#! /usr/bin/env python
#
# SCons - a Software Constructor
#
# Copyright (c) 2001 - 2019 The SCons Foundation
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

'''Show or convert the configuration of an SCons cache directory.

A cache of derived files is stored by file signature.
The files are split into directories named by the first few
digits of the signature. The prefix length used for directory
names can be changed by this script.
'''

from __future__ import print_function
import argparse
import glob
import json
import os

__revision__ = "src/script/scons-configure-cache.py 72ae09dc35ac2626f8ff711d8c4b30b6138e08e3 2019-08-08 14:50:06 bdeegan"

__version__ = "3.1.1"

__build__ = "72ae09dc35ac2626f8ff711d8c4b30b6138e08e3"

__buildsys__ = "octodog"

__date__ = "2019-08-08 14:50:06"

__developer__ = "bdeegan"


def rearrange_cache_entries(current_prefix_len, new_prefix_len):
    '''Move cache files if prefix length changed.

    Move the existing cache files to new directories of the
    appropriate name length and clean up the old directories.
    '''
    print('Changing prefix length from', current_prefix_len,
          'to', new_prefix_len)
    dirs = set()
    old_dirs = set()
    for file in glob.iglob(os.path.join('*', '*')):
        name = os.path.basename(file)
        dname = name[:current_prefix_len].upper()
        if dname not in old_dirs:
            print('Migrating', dname)
            old_dirs.add(dname)
        dname = name[:new_prefix_len].upper()
        if dname not in dirs:
            os.mkdir(dname)
            dirs.add(dname)
        os.rename(file, os.path.join(dname, name))

    # Now delete the original directories
    for dname in old_dirs:
        os.rmdir(dname)


# The configuration dictionary should have one entry per entry in the
# cache config. The value of each entry should include the following:
#   implicit - (optional) This is to allow adding a new config entry and also
#              changing the behaviour of the system at the same time. This
#              indicates the value the config entry would have had if it had
#              been specified.
#   default - The value the config entry should have if it wasn't previously
#             specified
#   command-line - parameters to pass to ArgumentParser.add_argument
#   converter - (optional) Function to call if conversion is required
#               if this configuration entry changes
config_entries = {
    'prefix_len': {
        'implicit': 1,
        'default': 2,
        'command-line': {
            'help': 'Length of cache file name used as subdirectory prefix',
            'metavar': '<number>',
            'type': int
        },
        'converter': rearrange_cache_entries
    }
}

parser = argparse.ArgumentParser(
    description='Modify the configuration of an scons cache directory',
    epilog='''
           Unspecified options will not be changed unless they are not
           set at all, in which case they are set to an appropriate default.
           ''')

parser.add_argument('cache-dir', help='Path to scons cache directory')
for param in config_entries:
    parser.add_argument('--' + param.replace('_', '-'),
                        **config_entries[param]['command-line'])
parser.add_argument('--version',
                    action='version',
                    version='%(prog)s 1.0')
parser.add_argument('--show',
                    action="store_true",
                    help="show current configuration")

# Get the command line as a dict without any of the unspecified entries.
args = dict([x for x in vars(parser.parse_args()).items() if x[1]])

# It seems somewhat strange to me, but positional arguments don't get the -
# in the name changed to _, whereas optional arguments do...
cache = args['cache-dir']
if not os.path.isdir(cache):
    raise RuntimeError("There is no cache directory named %s" % cache)
os.chdir(cache)
del args['cache-dir']

if not os.path.exists('config'):
    # old config dirs did not have a 'config' file. Try to update.
    # Validate the only files in the directory are directories 0-9, a-f
    expected = ['{:X}'.format(x) for x in range(0, 16)]
    if not set(os.listdir('.')).issubset(expected):
        raise RuntimeError(
            "%s does not look like a valid version 1 cache directory" % cache)
    config = dict()
else:
    with open('config') as conf:
        config = json.load(conf)

if args.get('show', None):
    print("Current configuration in '%s':" % cache)
    print(json.dumps(config, sort_keys=True,
                     indent=4, separators=(',', ': ')))
    # in case of the show argument, emit some stats as well
    file_count = 0
    for _, _, files in os.walk('.'):
        file_count += len(files)
    if file_count:  # skip config file if it exists
        file_count -= 1
    print("Cache contains %s files" % file_count)
    del args['show']

# Find any keys that are not currently set but should be
for key in config_entries:
    if key not in config:
        if 'implicit' in config_entries[key]:
            config[key] = config_entries[key]['implicit']
        else:
            config[key] = config_entries[key]['default']
        if key not in args:
            args[key] = config_entries[key]['default']

# Now go through each entry in args to see if it changes an existing config
# setting.
for key in args:
    if args[key] != config[key]:
        if 'converter' in config_entries[key]:
            config_entries[key]['converter'](config[key], args[key])
        config[key] = args[key]

# and write the updated config file
with open('config', 'w') as conf:
    json.dump(config, conf)
