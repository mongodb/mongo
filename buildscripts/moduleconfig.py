"""Utility functions for SCons to discover and configure
MongoDB modules (sub-trees of db/modules/). This file exports
two functions:

    discover_modules, which returns a dictionary of module name
        to the imported python module object for the module's
        build.py file

    configure_modules, which runs per-module configuration, and
        is given the SCons environment, its own path, etc

Each module must have a "build.py" script, which is expected to
have a "configure" function, and optionally a "test" function
if the module exposes per-module tests.
"""

__all__ = ('discover_modules', 'configure_modules')

import imp
from os import listdir
from os.path import abspath, dirname, join, isdir, isfile

def discover_modules(mongo_root):
    """Scan <mongo_root>/db/modules/ for directories that
    look like MongoDB modules (i.e. they contain a "build.py"
    file), and return a dictionary of module name (the directory
    name) to build.py python modules.
    """
    found_modules = {}

    module_root = abspath(join(mongo_root, 'db', 'modules'))
    if not isdir(module_root):
        return found_modules

    for name in listdir(module_root):
        root = join(module_root, name)
        if '.' in name or not isdir(root):
            continue

        build_py = join(root, 'build.py')
        module = None

        if isfile(build_py):
            print "adding module: %s" % name
            fp = open(build_py, "r")
            module = imp.load_module("module_" + name, fp, build_py, (".py", "r", imp.PY_SOURCE))
            found_modules[name] = module
            fp.close()

    return found_modules

def configure_modules(modules, conf, env):
    """
    Run the configure() function in the build.py python modules
    for each module listed in the modules dictionary (as created
    by discover_modules). The configure() function should use the
    prepare the Mongo build system for building the module.

    build.py files may specify a "customIncludes" flag, which, if
    True, causes configure() to be called with three arguments:
    the SCons Configure() object, the SCons environment, and an
    empty list which should be modified in-place by the configure()
    function; if false, configure() is called with only the first
    two arguments, and the source files are discovered with a
    glob against the <module_root>/src/*.cpp.

    Returns a dictionary mapping module name to a list of source
    files to be compiled for the module.
    """
    source_map = {}

    for name, module in modules.items():
        print "configuring module: %s" % name

        root = dirname(module.__file__)
        module_sources = []

        if getattr(module, "customIncludes", False):
            # then the module configures itself and its
            # configure() takes 3 args
            module.configure(conf, env, module_sources)
        else:
            # else we glob the files in the module's src/
            # subdirectory, and its configure() takes 2 args
            module.configure(conf, env)
            module_sources.extend(Glob(join(root, "src/*.cpp")))

        if not module_sources:
            print "WARNING: no source files for module %s, module will not be built." % name
        else:
            source_map[name] = module_sources

    return source_map
