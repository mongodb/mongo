# -*- mode: python; -*-

import hashlib

# Default and alternative generator definitions go here.


# This is the key/value mapping that will be returned by the buildInfo command and
# printed by the --version command-line option to mongod.
# Each mapped value is in turn a dict consisting of:
#   key: <string>
#   value: <string>
#   inBuildInfo: <bool> : should it be included in buildInfo output
#   inVersion: <bool> : should it be included in --version output
# The `value` field will be passed through env.subst, so you can use any SCons variables you
# want to define them.
def default_buildinfo_environment_data():
    data = (
        (
            'distmod',
            '$MONGO_DISTMOD',
            True,
            True,
        ),
        (
            'distarch',
            '$MONGO_DISTARCH',
            True,
            True,
        ),
        (
            'cc',
            '$CC_VERSION',
            True,
            False,
        ),
        (
            'ccflags',
            '$CCFLAGS',
            True,
            False,
        ),
        (
            'cxx',
            '$CXX_VERSION',
            True,
            False,
        ),
        (
            'cxxflags',
            '$CXXFLAGS',
            True,
            False,
        ),
        (
            'linkflags',
            '$LINKFLAGS',
            True,
            False,
        ),
        (
            'target_arch',
            '$TARGET_ARCH',
            True,
            True,
        ),
        (
            'target_os',
            '$TARGET_OS',
            True,
            False,
        ),
        (
            'cppdefines',
            '$CPPDEFINES',
            True,
            False,
        ),
    )
    return {
        k: {'key': k, 'value': v, 'inBuildInfo': ibi, 'inVersion': iv}
        for k, v, ibi, iv in data
    }


# If you want buildInfo and --version to be relatively empty, set
# MONGO_BUILDINFO_ENVIRONMENT_DATA = empty_buildinfo_environment_data()
def empty_buildinfo_environment_data():
    return {}


# TODO: SERVER-69064 Improve default_variant_dir_generator in Build System
def default_variant_dir_generator(target, source, env, for_signature):

    if env.GetOption('cache') != None:
        return 'cached'

    # If an option should affect the variant directory, name it here.
    variant_options = [
        'opt',
        'dbg',
    ]

    # Hash the named options and their values, and take the first 8 characters of the hash as
    # the variant name
    hasher = hashlib.md5()
    for option in variant_options:
        hasher.update(option.encode('utf-8'))
        hasher.update(str(env.GetOption(option)).encode('utf-8'))
    variant_dir = str(hasher.hexdigest()[0:8])

    # If our option hash yields a well known hash, replace it with its name.
    known_variant_hashes = {
        '343e6678': 'debug',
        '85fcf9b0': 'opt',
        '981ce870': 'debug',
        '9fface73': 'optdebug',
        'c52b1cc3': 'opt',
    }

    return known_variant_hashes.get(variant_dir, variant_dir)


def os_specific_variant_dir_generator(target, source, env, for_signature):
    return '-'.join([
        env['TARGET_OS'],
        default_variant_dir_generator(target, source, env, for_signature),
    ])
