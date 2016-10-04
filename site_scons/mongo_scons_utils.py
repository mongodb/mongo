import md5
import subprocess
import SCons.Action

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
    hasher = md5.md5()
    for option in variant_options:
        hasher.update(option)
        hasher.update(str(env.GetOption(option)))
    variant_dir = hasher.hexdigest()[0:8]

    # If our option hash yields a well known hash, replace it with its name.
    known_variant_hashes = {
        '343e6678' : 'debug',
        '85fcf9b0' : 'opt',
        '981ce870' : 'debug',
        '9fface73' : 'optdebug',
        'c52b1cc3' : 'opt',
    }

    return known_variant_hashes.get(variant_dir, variant_dir)


def os_specific_variant_dir_generator(target, source, env, for_signature):
    return '-'.join([
        env['TARGET_OS'],
        default_variant_dir_generator(target, source, env, for_signature)])

def get_toolchain_ver(env, tool):
    # By default we don't know the version of each tool, and only report what
    # command gets executed (gcc vs /opt/mongodbtoolchain/bin/gcc).
    verstr = "version unknown"
    proc = None
    if env.ToolchainIs('clang', 'gcc'):
        proc = SCons.Action._subproc(env,
            env.subst("${%s} --version" % tool),
            stdout=subprocess.PIPE,
            stderr='devnull',
            stdin='devnull',
            universal_newlines=True,
            error='raise',
            shell=True)
        verstr = proc.stdout.readline()

    elif env.ToolchainIs('msvc') and env.TargetOSIs('windows'):
        proc = SCons.Action._subproc(env,
            env.subst("${%s}" % tool),
            stdout='devnull',
            stderr=subprocess.PIPE,
            stdin='devnull',
            universal_newlines=True,
            error='raise',
            shell=True)
        verstr = proc.stderr.readline()

    # If we started a process, we should drain its stdout/stderr and wait for
    # it to end.
    if proc:
        proc.communicate()

    return env.subst('${%s}: %s' % (tool, verstr))

# This is the tuple that will be returned by the buildInfo command and
# printed by the --version command-line option to mongod.
# Each tuple consists of:
#   key (string)
#   value (string)
#   should be included in buildInfo output (bool)
#   should be included in --version output (bool)
# The values will be passed through env.subst, so you can use any SCons variables you
# want to define them.
def default_buildinfo_environment_data():
    return (
        ('distmod', '$MONGO_DISTMOD', True, True,),
        ('distarch', '$MONGO_DISTARCH', True, True,),
        ('cc', '$CC_VERSION', True, False,),
        ('ccflags', '$CCFLAGS', True, False,),
        ('cxx', '$CXX_VERSION', True, False,),
        ('cxxflags', '$CXXFLAGS', True, False,),
        ('linkflags', '$LINKFLAGS', True, False,),
        ('target_arch', '$TARGET_ARCH', True, True,),
        ('target_os', '$TARGET_OS', True, False,),
    )

# If you want buildInfo and --version to be relatively empty, set
# env['MONGO_BUILDINFO_ENVIRONMENT_DATA'] = empty_buildinfo_environment_data()
def empty_buildinfo_environment_data():
    return ()
