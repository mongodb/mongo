# -*- mode: python; -*-

"""
Support code related to OS detection in general. System specific facilities or customization
hooks live in mongo_platform_<PLATFORM>.py files.
"""

import os

# --- OS identification ---
#
# This needs to precede the options section so that we can only offer some options on certain
# operating systems.

# This function gets the running OS as identified by Python
# It should only be used to set up defaults for options/variables, because
# its value could potentially be overridden by setting TARGET_OS on the
# command-line. Treat this output as the value of HOST_OS
def get_running_os_name():
    running_os = os.sys.platform
    if running_os.startswith('linux'):
        running_os = 'linux'
    elif running_os.startswith('freebsd'):
        running_os = 'freebsd'
    elif running_os.startswith('openbsd'):
        running_os = 'openbsd'
    elif running_os == 'sunos5':
        running_os = 'solaris'
    elif running_os == 'win32':
        running_os = 'windows'
    elif running_os == 'darwin':
        running_os = 'macOS'
    else:
        running_os = 'unknown'
    return running_os

def env_get_os_name_wrapper(self):
    return self['TARGET_OS']

def is_os_raw(target_os, os_list_to_check):
    okay = False

    darwin_os_list = [ 'macOS', 'tvOS', 'tvOS-sim', 'iOS', 'iOS-sim' ]
    posix_os_list = [ 'linux', 'openbsd', 'freebsd', 'solaris' ] + darwin_os_list

    for p in os_list_to_check:
        if p == 'posix' and target_os in posix_os_list:
            okay = True
            break
        if p == 'darwin' and target_os in darwin_os_list:
            okay = True
            break
        elif p == target_os:
            okay = True
            break
    return okay

# This function tests the running OS as identified by Python
# It should only be used to set up defaults for options/variables, because
# its value could potentially be overridden by setting TARGET_OS on the
# command-line. Treat this output as the value of HOST_OS
def is_running_os(*os_list):
    return is_os_raw(get_running_os_name(), os_list)

def env_os_is_wrapper(self, *os_list):
    return is_os_raw(self['TARGET_OS'], os_list)
