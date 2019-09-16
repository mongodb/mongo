# Copyright 2019 MongoDB Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# TODO: Versioned libraries
# TODO: library dependency chaining for windows dynamic builds, static dev packages
# TODO: Injectible component dependencies (jscore -> resmoke, etc.)
# TODO: Handle chmod state
# TODO: Installing resmoke and configurations
# TODO: package decomposition
# TODO: Install/package target help text
# TODO: implement sdk_headers

import os
import sys
import shlex
import itertools
from collections import defaultdict, namedtuple

import SCons
from SCons.Tool import install

ALIAS_MAP = "AIB_ALIAS_MAP"
BASE_ROLE = "AIB_BASE_ROLE"
COMPONENTS = "AIB_COMPONENTS_EXTRA"
INSTALL_ACTIONS = "AIB_INSTALL_ACTIONS"
META_ROLE = "AIB_META_ROLE"
PACKAGE_ALIAS_MAP = "AIB_PACKAGE_ALIAS_MAP"
PACKAGE_PREFIX = "AIB_PACKAGE_PREFIX"
PRIMARY_COMPONENT = "AIB_COMPONENT"
PRIMARY_ROLE = "AIB_ROLE"
ROLES = "AIB_ROLES"
ROLE_DECLARATIONS = "AIB_ROLE_DECLARATIONS"
SUFFIX_MAP = "AIB_SUFFIX_MAP"

AIB_MAKE_ARCHIVE_CONTENT = """
import os
import sys
from shutil import which

USAGE = '''
Usage: {} ARCHIVE_TYPE ARCHIVE_NAME ROOT_DIRECTORY FILES...

FILES should be absolute paths or relative to ROOT_DIRECTORY.

ARCHIVE_TYPE is one of zip or tar.
'''
            
if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(sys.argv[0], "takes at minimum four arguments.")
        print(USAGE.format(sys.argv[0]))
        sys.exit(1)

    archive_type = sys.argv[1]
    archive_name = sys.argv[2]
    root_dir = sys.argv[3]
    files = sys.argv[4:]

    if archive_type not in ("zip", "tar"):
        print("unsupported archive_type", archive_type)
        print(USAGE.format(sys.argv[0]))
        sys.exit(1)

    if archive_type == "tar" and which("tar") is not None:
        import subprocess
        import shlex
        tar = which("tar")
        tar_cmd = "{tar} -C {root_dir} -czf {archive_name} {files}".format(
            tar=tar,
            root_dir=root_dir,
            archive_name=archive_name,
            files=" ".join(files),
        )
        subprocess.run(shlex.split(tar_cmd))
        sys.exit(0)

    if archive_type == "zip":
        import zipfile
        archive = zipfile.ZipFile(archive_name, mode='w', compression=zipfile.ZIP_DEFLATED)
        add_file = archive.write
    else:
        import tarfile
        archive = tarfile.open(archive_name, mode='w:gz')
        add_file = archive.add

    os.chdir(root_dir)

    for filename in files:
        add_file(filename)

    archive.close()
"""

RoleInfo = namedtuple(
    'RoleInfo',
    [
        'alias_name',
        'alias',
    ],
)

SuffixMap = namedtuple(
    'SuffixMap',
    [
        'directory',
        'default_roles',
    ],
)

class DeclaredRole():
    def __init__(self, name, dependencies=None, transitive=False, silent=False):
        self.name = name

        if dependencies is None:
            self.dependencies = set()
        else:
            self.dependencies = {dep for dep in dependencies if dep is not None}

        self.transitive = transitive
        self.silent = silent

def declare_role(env, **kwargs):
    """Construct a new role declaration"""
    return DeclaredRole(**kwargs)

def declare_roles(env, roles, base_role=None, meta_role=None):
    """Given a list of role declarations, validate them and store them in the environment"""

    role_names = [role.name for role in roles]
    if len(role_names) != len(set(role_names)):
        raise Exception(
            "Cannot declare duplicate roles"
        )

    # Ensure that all roles named in dependency lists actually were
    # passed in as a role.
    for role in roles:
        for d in role.dependencies:
            if d not in role_names:
                raise Exception(
                    "Role dependency '{}' does not name a declared role".format(d)
                )

    if isinstance(base_role, str):
        if base_role not in role_names:
            raise Exception(
                "A base_role argument was provided but it does not name a declared role"
            )
    elif isinstance(base_role, DeclaredRole):
        if base_role not in roles:
            raise Exception(
                "A base_role argument was provided but it is not a declared role"
            )
    elif base_role is not None:
        raise Exception(
            "The base_role argument must be a string name of a role or a role object"
        )
    else:
        # Set it to something falsy
        base_role = str()

    if isinstance(meta_role, str):
        if meta_role not in role_names:
            raise Exception(
                "A meta_role argument was provided but it does not name a declared role"
            )
    elif isinstance(meta_role, DeclaredRole):
        if meta_role not in roles:
            raise Exception(
                "A meta_role argument was provided but it is not a declared role"
            )
    elif meta_role is not None:
        raise Exception(
            "The meta_role argument must be a string name of a role or a role object"
        )
    else:
        # Set it to something falsy
        meta_role = str()

    silents = [role for role in roles if role.silent]
    if len(silents) > 1:
        raise Exception(
            "No more than one role can be declared as silent"
        )

    # If a base role was given, then add it as a dependency of every
    # role that isn't the base role (which would be circular).
    if base_role:
        for role in roles:
            if role.name != base_role:
                role.dependencies.add(base_role)

    # Become a dictionary, so we can look up roles easily.
    roles = { role.name : role for role in roles }

    # If a meta role was given, then add every role which isn't the
    # meta role as one of its dependencies.
    if meta_role:
        roles[meta_role].dependencies.update(r for r in roles.keys() if r != meta_role)

    # TODO: Check for DAG

    # TODO: What if base_role or meta_role is really None?
    env[BASE_ROLE] = base_role
    env[META_ROLE] = meta_role
    env[ROLE_DECLARATIONS] = roles


def generate_alias(env, component, role, target="install"):
    """Generate a scons alias for the component and role combination"""
    return "{target}-{component}{role}".format(
        target=target,
        component=component,
        role="" if env[ROLE_DECLARATIONS][role].silent else "-" + role,
    )


def get_package_name(env, component, role):
    """Return the package file name for the component and role combination."""
    basename = env[PACKAGE_ALIAS_MAP].get(
        (component, role),
        "{component}-{role}".format(component=component, role=role)
    )
    return "${{{prefix}}}{basename}".format(basename=basename, prefix=PACKAGE_PREFIX)


def get_dependent_actions(
        env,
        components,
        roles,
        non_transitive_roles,
        node,
        cb=None,
):
    """
    Check if node is a transitive dependency of components and roles

    If cb is not None and is callable then it will be called with all
    the arguments that get_dependent_actions was called with (except
    for cb itself) as well as the results of node_roles and the
    aib_install_actions that this function would have returned. The
    return of cb should be the dependent actions. This allows cb to
    access the results of scanning and modify the returned results via
    additional filtering.

    Returns the dependent actions.
    """
    actions = getattr(node.attributes, INSTALL_ACTIONS, None)
    if not actions:
        return []

    # Determine if the roles have any overlap with non_transitive_roles
    #
    # If they are overlapping then that means we can't transition to a
    # new role during scanning.
    if env[BASE_ROLE] not in roles:
        can_transfer = (
            non_transitive_roles
            and roles.isdisjoint(non_transitive_roles)
        )
    else:
        can_transfer = True

    node_roles = {
        role for role
        in getattr(node.attributes, ROLES, set())
        if role != env[META_ROLE]
    }
    if (
        # TODO: make the "always transitive" roles configurable
        env[BASE_ROLE] not in node_roles
        # If we are not transferrable
        and not can_transfer
        # Checks if we are actually crossing a boundry
        and node_roles.isdisjoint(roles)
    ):
        return []

    if cb is not None and callable(cb):
        return cb(
            components,
            roles,
            non_transitive_roles,
            node,
            node_roles,
            actions,
        )
    return actions


def scan_for_transitive_install(node, env, cb=None):
    """Walk the children of node finding all installed dependencies of it."""
    results = []
    install_sources = node.sources
    # Filter out all
    components = {
        component for component
        in getattr(node.sources[0].attributes, COMPONENTS, set())
        if component != "all"
    }
    roles = {
        role for role
        in getattr(node.sources[0].attributes, ROLES, set())
        if role != env[META_ROLE]
    }

    non_transitive_roles = {role for role in roles if env[ROLE_DECLARATIONS][role].transitive}
    for install_source in install_sources:
        install_executor = install_source.get_executor()
        if not install_executor:
            continue
        install_targets = install_executor.get_all_targets()
        if not install_targets:
            continue
        for install_target in install_targets:
            grandchildren = install_target.children()
            for grandchild in grandchildren:
                results.extend(
                    get_dependent_actions(
                        env,
                        components,
                        roles,
                        non_transitive_roles,
                        grandchild,
                        cb=cb,
                    )
                )

    # Produce deterministic output for caching purposes
    results = sorted(results, key=str)
    return results


def collect_transitive_files(env, source):
    """Collect all transitive files for source where source is a list of either Alias or File nodes."""
    files = []

    for s in source:
        if isinstance(s, SCons.Node.FS.File):
            files.append(s)
        else:
            files.extend(collect_transitive_files(env, s.children()))

    return files


def archive_builder(source, target, env, for_signature):
    """Build archives of the AutoInstall'd sources."""
    if not source:
        return

    source = env.Flatten([source])
    common_ancestor = None

    archive_type = env["__AIB_ARCHIVE_TYPE"]
    make_archive_script = source[0].get_abspath()
    source = source[1:]

    # Get the path elements that make up both DESTDIR and PREFIX. Then
    # iterate the dest_dir_elems with the prefix path elements
    # stripped off the end of the path converting them to strings for
    # joining to make the common_ancestor.
    #
    # We pass the common_ancestor to tar via -C so that $PREFIX is
    # preserved in the tarball.
    dest_dir_elems = env.Dir("$DESTDIR").get_abspath()
    prefix_elems = env.subst("$PREFIX")
    # In python slicing a string with [:-0] gives an empty string. So
    # make sure we have a prefix to slice off before trying it.
    if prefix_elems:
        common_ancestor = dest_dir_elems[:-len(prefix_elems)]
    else:
        common_ancestor = dest_dir_elems

    # Pre-process what should be in the archive
    transitive_files = collect_transitive_files(env, source)
    paths = {file.get_abspath() for file in transitive_files}

    # The env["ESCAPE"] function is used by scons to make arguments
    # valid for the platform that we're running on. For instance it
    # will properly quote paths that have spaces in them on Posix
    # platforms and handle \ / on Windows.
    escape_func = env.get("ESCAPE", lambda x: x)
    relative_files = " ".join([
        escape_func(os.path.relpath(path, common_ancestor))
        for path in paths
    ])
    archive_name = env.File(target[0]).get_abspath()

    return "{python} {make_archive_script} {archive_type} {archive_name} {common_ancestor} {relative_files}".format(
        python=sys.executable,
        archive_type=archive_type,
        archive_name=archive_name,
        make_archive_script=make_archive_script,
        common_ancestor=common_ancestor,
        relative_files=relative_files,
    )



def auto_install(env, target, source, **kwargs):
    """Auto install builder."""
    source = [env.Entry(s) for s in env.Flatten([source])]
    roles = {
        kwargs.get(PRIMARY_ROLE),
    }

    if env[META_ROLE]:
        roles.add(env[META_ROLE])

    if kwargs.get(ROLES) is not None:
        roles = roles.union(set(kwargs[ROLES]))

    component = kwargs.get(PRIMARY_COMPONENT)
    if (
            component is not None
            and (not isinstance(component, str)
                 or " " in component)
    ):
        raise Exception(
            "AIB_COMPONENT must be a string and contain no whitespace."
        )

    components = {
        component,
        # The 'all' tag is implicitly attached as a component
        "all",
    }
    # Some tools will need to create multiple components so we add
    # this "hidden" argument that accepts a set or list.
    #
    # Use get here to check for existence because it is rarely
    # ommitted as a kwarg (because it is set by the default emitter
    # for all common builders), but is often set to None.
    if kwargs.get(COMPONENTS) is not None:
        components = components.union(set(kwargs[COMPONENTS]))

    # Remove false values such as None or ""
    roles = {role for role in roles if role}
    components = {component for component in components if component}

    actions = []

    for s in source:
        s.attributes.keep_targetinfo = 1
        setattr(s.attributes, COMPONENTS, components)
        setattr(s.attributes, ROLES, roles)

        # We must do an eearly subst here so that the _aib_debugdir
        # generator has a chance to run while seeing 'source'.
        #
        # TODO: Find a way to not need this early subst.
        target = env.Dir(env.subst(target, source=source))

        action = env.Install(
            target=target,
            source=s,
        )

        setattr(
            s.attributes,
            INSTALL_ACTIONS,
            action if isinstance(action, (list, set)) else [action]
        )
        actions.append(action)

    actions = env.Flatten(actions)
    for component, role in itertools.product(components, roles):
        alias_name = generate_alias(env, component, role)
        alias = env.Alias(alias_name, actions)
        setattr(alias[0].attributes, COMPONENTS, components)
        setattr(alias[0].attributes, ROLES, roles)

        # TODO: this hard codes behavior that should be done configurably
        if component != "common":
            # We have to call env.Alias just in case the
            # generated_alias does not already exist.
            env.Depends(alias, env.Alias(generate_alias(env, "common", role)))

        env[ALIAS_MAP][component][role] = RoleInfo(
            alias_name=alias_name,
            alias=alias,
        )

    return actions


def finalize_install_dependencies(env):
    """Generates package aliases and wires install dependencies."""
    common_rolemap = env[ALIAS_MAP].get("common")
    default_rolemap = env[ALIAS_MAP].get("default")

    if default_rolemap and "runtime" in default_rolemap:
        env.Alias("install", "install-default")
        env.Default("install")

    # TODO: $BUILD_ROOT should be $VARIANT_DIR after we fix our dir
    # setup later on.
    make_archive_script = env.Textfile(
        target="$BUILD_ROOT/aib_make_archive.py",
        source=[AIB_MAKE_ARCHIVE_CONTENT],
    )

    for component, rolemap in env[ALIAS_MAP].items():
        for role, info in rolemap.items():

            aliases = [info.alias]
            if common_rolemap and component != "common" and role in common_rolemap:
                env.Depends(info.alias, common_rolemap[role].alias)
                aliases.extend(common_rolemap[role].alias)

            role_decl = env[ROLE_DECLARATIONS].get(role)
            for dependency in role_decl.dependencies:
                dependency_info = rolemap.get(dependency, [])
                if dependency_info:
                    env.Depends(info.alias, dependency_info.alias)

            pkg_name = get_package_name(env, component, role)

            for fmt in ("zip", "tar"):
                # TODO: $PKGDIR support
                if fmt == "zip":
                    pkg_suffix = "$AIB_ZIP_SUFFIX"
                else:
                    pkg_suffix = "$AIB_TARBALL_SUFFIX"

                archive = env.__AibArchive(
                    target="#{}.{}".format(pkg_name, pkg_suffix),
                    source=[make_archive_script] + aliases,
                    __AIB_ARCHIVE_TYPE=fmt,
                    AIB_COMPONENT=component,
                    AIB_ROLE=role,
                )

                # TODO: perhaps caching of packages / tarballs should be
                # configurable? It's possible someone would want to do it.
                env.NoCache(archive)

                archive_alias = generate_alias(env, component, role, target=fmt)
                env.Alias(archive_alias, archive)


def auto_install_emitter(target, source, env):
    """When attached to a builder adds an appropriate AutoInstall to that Builder."""
    for t in target:
        entry = env.Entry(t)
        suffix = entry.get_suffix()
        if env.get("AIB_IGNORE", False):
            continue
        auto_install_mapping = env[SUFFIX_MAP].get(suffix)
        if auto_install_mapping is not None:
            env.AutoInstall(
                auto_install_mapping.directory,
                entry,
                AIB_COMPONENT=env.get(PRIMARY_COMPONENT),
                AIB_ROLE=env.get(PRIMARY_ROLE),
                AIB_ROLES=auto_install_mapping.default_roles,
                AIB_COMPONENTS_EXTRA=env.get(COMPONENTS),
            )
    return (target, source)


def add_suffix_mapping(env, suffix, role=None):
    """Map suffix to role"""
    if isinstance(suffix, str):
        if role not in env[ROLE_DECLARATIONS]:
            raise Exception(
                "target {} is not a known role. Available roles are {}".format(
                    role, env[ROLE_DECLARATIONS].keys()
                )
            )
        env[SUFFIX_MAP][env.subst(suffix)] = role

    if not isinstance(suffix, dict):
        raise Exception("source must be a dictionary or a string")

    for _, mapping in suffix.items():
        for role in mapping.default_roles:
            if role not in env[ROLE_DECLARATIONS]:
                raise Exception(
                    "target {} is not a known role. Available roles are {}".format(
                        target, env[ROLE_DECLARATIONS].keys()
                    )
                )

    env[SUFFIX_MAP].update({env.subst(key): value for key, value in suffix.items()})


def add_package_name_alias(env, component, role, name):
    """Add a package name mapping for the combination of component and role."""
    # Verify we didn't get a None or empty string for any argument
    if not name:
        raise Exception("when setting a package name alias must provide a name parameter")
    if not component:
        raise Exception("No component provided for package name alias")
    if not role:
        raise Exception("No role provided for package name alias")
    env[PACKAGE_ALIAS_MAP][(component, role)] = name

def suffix_mapping(env, directory=False, default_roles=False):
    """Generate a SuffixMap object from source and target."""
    return SuffixMap(
        directory=directory,
        default_roles=default_roles,
    )

def dest_dir_generator(initial_value=None):
    """Memoized dest_dir_generator"""
    dd = (None, None)

    def generator(source, target, env, for_signature):
        nonlocal dd

        # SCons does not perform substitution for "sub" Dir calls on a
        # Dir Node. Additionally we need to determine if it's an
        # absolute path here because if it is the sub Dir call will
        # not expand correctly.
        prefix = env.subst("$PREFIX")
        if prefix and prefix[0] == '/':
            prefix = prefix[1:]

        if dd[1] is not None and dd[0] == prefix:
            return dd[1]

        if initial_value is None:
            dest_dir = env.Dir("#install")
        elif isinstance(initial_value, str):
            dest_dir = env.Dir(initial_value) 
        elif isinstance(initial_value, SCons.Node.FS.Dir):
            dest_dir = initial_value
        else:
            raise Exception("initial DESTDIR value must be string or Dir")

        dd = (prefix, dest_dir.Dir(prefix))
        return dd[1]

    return generator
    

def _aib_debugdir(source, target, env, for_signature):
    for s in source:
        # TODO: We shouldn't need to reach into the attributes of the debug tool like this.
        origin = getattr(s.attributes, "debug_file_for", None)
        oentry = env.Entry(origin)
        osuf = oentry.get_suffix()
        return env[SUFFIX_MAP].get(osuf)[0]


def exists(_env):
    """Always activate this tool."""
    return True


def list_components(env, **kwargs):
    """List registered components for env."""
    print("Known AIB components:")
    for key in env[ALIAS_MAP]:
        print("\t", key)


def list_targets(env, **kwargs):
    """List AIB generated targets for env."""
    print("Generated AIB targets:")
    for _, rolemap in env[ALIAS_MAP].items():
        for _, info in rolemap.items():
            print("\t", info.alias[0].name)


def generate(env):  # pylint: disable=too-many-statements
    """Generate the auto install builders."""
    bld = SCons.Builder.Builder(
        action=SCons.Action.CommandGeneratorAction(
            archive_builder,
           {"cmdstr": "Building package ${TARGETS[0]} from ${SOURCES[1:]}"},
        )
    )
    env.Append(BUILDERS={"__AibArchive": bld})
    env["AIB_TARBALL_SUFFIX"] = env.get("AIB_TARBALL_SUFFIX", "tar.gz")
    env["AIB_ZIP_SUFFIX"] = env.get("AIB_ZIP_SUFFIX", "zip")

    # Matches the autoconf documentation:
    # https://www.gnu.org/prep/standards/html_node/Directory-Variables.html
    env["DESTDIR"] = dest_dir_generator(env.get("DESTDIR", None))
    env["PREFIX_BINDIR"] = env.get("PREFIX_BINDIR", "$DESTDIR/bin")
    env["PREFIX_LIBDIR"] = env.get("PREFIX_LIBDIR", "$DESTDIR/lib")
    env["PREFIX_SHAREDIR"]  = env.get("PREFIX_SHAREDIR", "$DESTDIR/share")
    env["PREFIX_DOCDIR"] = env.get("PREFIX_DOCDIR", "$PREFIX_SHAREDIR/doc")
    env["PREFIX_INCLUDEDIR"] = env.get("PREFIX_INCLUDEDIR", "$DESTDIR/include")
    env["PREFIX_DEBUGDIR"] = env.get("PREFIX_DEBUGDIR", _aib_debugdir)
    env[PACKAGE_PREFIX] = env.get(PACKAGE_PREFIX, "")
    env[SUFFIX_MAP] = {}
    env[PACKAGE_ALIAS_MAP] = {}
    env[ALIAS_MAP] = defaultdict(dict)

    env.AddMethod(suffix_mapping, "SuffixMap")
    env.AddMethod(add_suffix_mapping, "AddSuffixMapping")
    env.AddMethod(add_package_name_alias, "AddPackageNameAlias")
    env.AddMethod(auto_install, "AutoInstall")
    env.AddMethod(finalize_install_dependencies, "FinalizeInstallDependencies")
    env.AddMethod(declare_role, "Role")
    env.AddMethod(declare_roles, "DeclareRoles")
    env.Tool("install")

    # TODO: we should probably expose these as PseudoBuilders and let
    # users define their own aliases for them.
    env.Alias("list-aib-components", [], [list_components])
    env.AlwaysBuild("list-aib-components")

    env.Alias("list-aib-targets", [], [list_targets])
    env.AlwaysBuild("list-aib-targets")

    for builder in ["Program", "SharedLibrary", "LoadableModule", "StaticLibrary"]:
        builder = env["BUILDERS"][builder]
        base_emitter = builder.emitter
        # TODO: investigate if using a ListEmitter here can cause
        # problems if AIB is not loaded last
        new_emitter = SCons.Builder.ListEmitter([
            base_emitter,
            auto_install_emitter,
        ])
        builder.emitter = new_emitter

    base_install_builder = install.BaseInstallBuilder
    assert base_install_builder.target_scanner is None

    base_install_builder.target_scanner = SCons.Scanner.Scanner(
        function=scan_for_transitive_install, path_function=None
    )
