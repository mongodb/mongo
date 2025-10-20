"""Utility functions for SCons to discover and configure MongoDB modules.

A MongoDB module is an organized collection of source code and build rules that can be provided at
compile-time to alter or extend the behavior of MongoDB.  The files comprising a single MongoDB
module are arranged in a directory hierarchy, rooted in a directory whose name is by convention the
module name, and containing in that root directory at least two files: a build.py file and a
SConscript file.

MongoDB modules are discovered by a call to the discover_modules() function, whose sole parameter is
the directory which is the immediate parent of all module directories.  The exact directory is
chosen by the SConstruct file, which is the direct consumer of this python module.  The only rule is
that it must be a subdirectory of the src/ directory, to correctly work with the SCons variant
directory system that separates build products for source.

Once discovered, modules are configured by the configure_modules() function, and the build system
integrates their SConscript files into the rest of the build.

MongoDB module build.py files implement a single function, configure(conf, env), which they may use
to configure the supplied "env" object.  The configure functions may add extra LIBDEPS to mongod,
mongos and the mongo shell (TODO: other mongo tools and the C++ client), and through those libraries
alter those programs' behavior.

MongoDB module SConscript files can describe libraries, programs and unit tests, just as other
MongoDB SConscript files do.
"""

__all__ = ('discover_modules', 'discover_module_directories', 'configure_modules',
           'register_module_test')  # pylint: disable=undefined-all-variable

import importlib
import inspect
import os


def discover_modules(module_root, allowed_modules):
    """Scan module_root for subdirectories that look like MongoDB modules.

    Return a list of imported build.py module objects.
    """
    found_modules = []
    found_module_names = []

    if allowed_modules is not None:
        allowed_modules = allowed_modules.split(',')
        # When `--modules=` is passed, the split on empty string is represented
        # in memory as ['']
        if allowed_modules == ['']:
            allowed_modules = []

    if not os.path.isdir(module_root):
        if allowed_modules:
            raise RuntimeError(
                f"Requested the following modules: {allowed_modules}, but the module root '{module_root}' could not be found. Check the module root, or remove the module from the scons invocation."
            )
        return found_modules

    for name in os.listdir(module_root):
        root = os.path.join(module_root, name)
        if name.startswith('.') or not os.path.isdir(root):
            continue

        build_py = os.path.join(root, 'build.py')
        module = None

        if allowed_modules is not None and name not in allowed_modules:
            print("skipping module: %s" % (name))
            continue

        try:
            print("adding module: %s" % (name))
            spec = importlib.util.spec_from_file_location("module_" + name, build_py)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)

            if getattr(module, "name", None) is None:
                module.name = name
            found_modules.append(module)
            found_module_names.append(name)
        except (FileNotFoundError, IOError):
            pass

    if allowed_modules is not None:
        missing_modules = set(allowed_modules) - set(found_module_names)
        if missing_modules:
            raise RuntimeError(f"Failed to locate all modules. Could not find: {missing_modules}")

    return found_modules


def discover_module_directories(module_root, allowed_modules):
    """Scan module_root for subdirectories that look like MongoDB modules.

    Return a list of directory names.
    """
    if not os.path.isdir(module_root):
        return []

    found_modules = []

    if allowed_modules is not None:
        allowed_modules = allowed_modules.split(',')

    for name in os.listdir(module_root):
        root = os.path.join(module_root, name)
        if name.startswith('.') or not os.path.isdir(root):
            continue

        build_py = os.path.join(root, 'build.py')

        if allowed_modules is not None and name not in allowed_modules:
            print("skipping module: %s" % (name))
            continue

        if os.path.isfile(build_py):
            print("adding module: %s" % (name))
            found_modules.append(name)

    return found_modules


def configure_modules(modules, conf):
    """Run the configure() function in the build.py python modules for each module in "modules".

    The modules were created by discover_modules.

    The configure() function should prepare the Mongo build system for building the module.
    """
    env = conf.env
    env['MONGO_MODULES'] = []
    for module in modules:
        name = module.name
        print("configuring module: %s" % (name))
        modules_configured = module.configure(conf, env)
        if modules_configured:
            for module_name in modules_configured:
                env['MONGO_MODULES'].append(module_name)
        else:
            env['MONGO_MODULES'].append(name)


def get_module_sconscripts(modules):
    """Return all modules' sconscripts."""
    sconscripts = []
    for mod in modules:
        module_dir_path = __get_src_relative_path(os.path.join(os.path.dirname(mod.__file__)))
        sconscripts.append(os.path.join(module_dir_path, 'SConscript'))
    return sconscripts


def __get_src_relative_path(path):
    """Return a path relative to ./src.

    The src directory is important because of its relationship to BUILD_DIR,
    established in the SConstruct file.  For variant directories to work properly
    in SCons, paths relative to the src or BUILD_DIR must often be generated.
    """
    src_dir = os.path.abspath('src')
    path = os.path.abspath(os.path.normpath(path))
    if not path.startswith(src_dir):
        raise ValueError('Path "%s" is not relative to the src directory "%s"' % (path, src_dir))
    result = path[len(src_dir) + 1:]
    return result


def __get_module_path(module_frame_depth):
    """Return the path to the MongoDB module whose build.py is executing "module_frame_depth" frames.

    This is above this function, relative to the "src" directory.
    """
    module_filename = inspect.stack()[module_frame_depth + 1][1]
    return os.path.dirname(__get_src_relative_path(module_filename))


def __get_module_src_path(module_frame_depth):
    """Return the path relative to the SConstruct file of the MongoDB module's source tree.

    module_frame_depth is the number of frames above the current one in which one can find a
    function from the MongoDB module's build.py function.
    """
    return os.path.join('src', __get_module_path(module_frame_depth + 1))


def __get_module_build_path(module_frame_depth):
    """Return the path relative to the SConstruct file of the MongoDB module's build tree.

    module_frame_depth is the number of frames above the current one in which one can find a
    function from the MongoDB module's build.py function.
    """
    return os.path.join('$BUILD_DIR', __get_module_path(module_frame_depth + 1))


def get_current_module_src_path():
    """Return the path relative to the SConstruct file of the current MongoDB module's source tree.

    May only meaningfully be called from within build.py
    """
    return __get_module_src_path(1)


def get_current_module_build_path():
    """Return the path relative to the SConstruct file of the current MongoDB module's build tree.

    May only meaningfully be called from within build.py
    """

    return __get_module_build_path(1)


def get_current_module_libdep_name(libdep_rel_path):
    """Return a $BUILD_DIR relative path to a "libdep_rel_path".

    The "libdep_rel_path" is relative to the MongoDB module's build.py file.

    May only meaningfully be called from within build.py
    """
    return os.path.join(__get_module_build_path(1), libdep_rel_path)
