import os

from jstoh import jsToHeader
from SCons.Builder import Builder

def jsToH(target, source, env):
    jsToHeader(str(target[0]), source)

jshBuilder = Builder( action=jsToH )

def generate(env, **kw):
    env.Append( BUILDERS=dict( JSHeader=jshBuilder ) )

def exists(env):
    return True
