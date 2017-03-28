from SCons.Script import Action

def jsToH(env, target, source):
    return env.Command(
        target=target,
        source=['#site_scons/site_tools/jstoh.py'] + source,
        action=Action('$PYTHON ${SOURCES[0]} $TARGET ${SOURCES[1:]}'))

def generate(env, **kw):
    env.AddMethod(jsToH, 'JSHeader')

def exists(env):
    return True
