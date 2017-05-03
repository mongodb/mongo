# -*- mode: python; -*-

import subprocess

import SCons

# Helper functions for generic toolchain things go here

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
