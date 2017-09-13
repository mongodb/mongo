import SCons

def generate(env):

    env['AR'] = 'libtool'
    env['ARCOM'] = '$AR -static -o $TARGET $ARFLAGS $SOURCES'
    env['ARFLAGS'] = ["-s", "-no_warning_for_no_symbols"]

    def noop_action(env, target, source):
        pass

    # Disable running ranlib, since we added 's' above
    env['RANLIBCOM'] = noop_action
    env['RANLIBCOMSTR'] = 'Skipping ranlib for libtool generated target $TARGET'

def exists(env):
    return env.detect('libtool')
