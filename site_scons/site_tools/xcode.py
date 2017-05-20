import os

def exists(env):
    return env.Detect('xcrun')

def generate(env):
    if not exists(env):
        return

    if 'DEVELOPER_DIR' in os.environ:
        env['ENV']['DEVELOPER_DIR'] = os.environ['DEVELOPER_DIR']
        print "NOTE: Xcode detected; propagating DEVELOPER_DIR from shell environment to subcommands"
