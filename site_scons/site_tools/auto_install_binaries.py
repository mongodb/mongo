# TODO: Versioned libraries
# TODO: library dependency chaining for windows dynamic builds, static dev packages
# TODO: Injectible component dependencies (jscore -> resmoke, etc.)
# TODO: Handle chmod state
# TODO: Installing resmoke and configurations
# TODO: package decomposition
# TODO: Install/package target help text
# TODO: implement sdk_headers

import os
import shlex
import itertools
from collections import defaultdict, namedtuple

import SCons
from SCons.Tool import install

ALIAS_MAP = 'AIB_ALIAS_MAP'
SUFFIX_MAP = 'AIB_SUFFIX_MAP'
ROLE_DEPENDENCIES = 'AIB_ROLE_DEPENDENCIES'
COMPONENTS = 'AIB_COMPONENTS_EXTRA'
ROLES = 'AIB_ROLES'

PRIMARY_COMPONENT = 'AIB_COMPONENT'
PRIMARY_ROLE = 'AIB_ROLE'

AVAILABLE_ROLES = [
    "base",
    "debug",
    "dev",
    "meta",
    "runtime",
]

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

def generate_alias(component, role, target="install"):
    return "{target}-{component}{role}".format(
        target=target,
        component=component,
        role="" if role == "runtime" else "-" + role,
    )


def get_dependent_actions(
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
    actions = getattr(node.attributes, "aib_install_actions", None)
    if not actions:
        return []

    # Determine if the roles have any overlap with non_transitive_roles
    #
    # If they are overlapping then that means we can't transition to a
    # new role during scanning.
    can_transfer = (
        non_transitive_roles
        and roles.isdisjoint(non_transitive_roles)
    )

    node_roles = {
        role for role
        in getattr(node.attributes, "aib_roles", set())
        if role != "meta"
    }
    if (
            # TODO: make the "always transitive" roles configurable
            "common" not in node_roles
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

def scan_for_transitive_install(node, env, path=None, cb=None):
    """Walk the children of node finding all installed dependencies of it."""
    results = []
    install_sources = node.sources
    # Filter out all
    components = {
        component for component
        in getattr(node.sources[0].attributes, "aib_components", set())
        if component != "all"
    }
    roles = {
        role for role
        in getattr(node.sources[0].attributes, "aib_roles", set())
        if role != "meta"
    }
    # TODO: add fancy configurability
    non_transitive_roles = {role for role in roles if role == "runtime"}
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


def collect_transitive_files(sources, env, ignore_component_boundries=False):
    """Collect all transitive files for node."""
    dependent_components = {}

    def check_component_boundries(
            components,
            roles,
            non_transitive_roles,
            node,
            node_roles,
            aib_actions,
    ):
        # Filter out all, otherwise it would always share a component
        node_components = {
            component for component
            in getattr(node.sources[0].attributes, "aib_components", set())
            if component != "all"
        }
        shares_component = not node_components.isdisjoint(components)
        if not shares_component:
            dependent_components = dependent_components.union(node_components)

        # TODO: make the always transitive roles configurable
        #
        # Check if we're crossing a component boundry. Unless we're one of
        # the "special" always transitive roles we should not cross
        # component boundries.
        if (
                not ignore_component_boundries
                and "common" not in node_roles
                and not shares_component
        ):
            return []
        return aib_actions

    scanned_components = set()
    files = []
    for source in sources:
        component = getattr(source.sources[0].attributes, "aib_components", set())
        if not component:
            continue

        diff = component.difference(scanned_components)
        if not diff:
            continue

        scanned_components = scanned_components.union(component)
        files.extend(scan_for_transitive_install(source, env))

    files.extend(sources)
    return {
        "dependent_components": dependent_components,
        "files": files,
    }

def tarball_builder(target, source, env):
    """Build a tarball of the AutoInstall'd sources."""
    if not source:
        return
    if not isinstance(source, list):
        source = [source]
    transitive_files = collect_transitive_files(
        source,
        env,
        ignore_component_boundries=True
    )
    common_ancestor = env.Dir("$DEST_DIR").get_abspath()
    paths = [file.get_abspath() for file in transitive_files["files"]]
    relative_files = [os.path.relpath(path, common_ancestor) for path in paths]
    tar_cmd = SCons.Action._subproc(
        env,
        shlex.split(
            'tar -cvf {tarball} -C {ancestor} {files}'
            .format(
                tarball=target[0],
                ancestor=common_ancestor,
                files=" ".join(relative_files),
            )
        ),
    )
    tar_cmd.wait()


def auto_install(env, target, source, **kwargs):
    """Auto install builder."""
    target = env.Dir(env.subst(target, source=source))
    source = list(map(env.Entry, env.Flatten([source])))
    roles = {
        kwargs.get(PRIMARY_ROLE),
        # The 'meta' tag is implicitly attached as a role.
        "meta",
    }

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

    actions = env.Install(
        target=target,
        source=source,
    )
    for s in source:
        s.attributes.keep_targetinfo = 1
        s.attributes.aib_install_actions = actions
        s.attributes.aib_components = components
        s.attributes.aib_roles = roles

    for component, role in itertools.product(components, roles):
        alias_name = generate_alias(component, role)
        alias = env.Alias(alias_name, actions)
        if role != "base":
            env.Depends(alias, env.Alias(generate_alias(component, "base")))
        if not (component == "common" and role == "base"):
            env.Depends(alias, env.Alias("install-common-base"))
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

    installed_files = env.FindInstalledFiles()

    for component, rolemap in env[ALIAS_MAP].items():
        for role, info in rolemap.items():

            if common_rolemap and component != "common" and role in common_rolemap:
                env.Depends(info.alias, common_rolemap[role].alias)

            for dependency in env[ROLE_DEPENDENCIES].get(role, []):
                dependency_info = rolemap.get(dependency, [])
                if dependency_info:
                    env.Depends(info.alias, dependency_info.alias)

            installed_component_files = [
                file for file in installed_files
                if (
                        component in getattr(file.sources[0].attributes, COMPONENTS, [])
                        and (
                            role in getattr(file.sources[0].attributes, ROLES, [])
                            # TODO: make configurable
                            or "common" in getattr(file.sources[0].attributes, ROLES, [])
                        )
                )
            ]

            tar_alias = generate_alias(component, role, target="tar")
            tar = env.TarBall(
                "{}-{}.tar".format(component, role),
                source=installed_component_files,
                AIB_COMPONENT=component,
                AIB_ROLE=role,
            )
            env.NoCache(tar)
            env.Alias(tar_alias, tar)
            env.Depends(tar, info.alias)


def auto_install_emitter(target, source, env):
    """When attached to a builder adds an appropriate AutoInstall to that Builder."""
    for t in target:
        entry = env.Entry(t)
        suffix = entry.get_suffix()
        if env.get('AIB_IGNORE', False):
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


def extend_attr(node, attr, value):
    """Set attr to value or extend the set if it exists."""
    existing = getattr(node.attributes, attr, False)
    if existing:
        value = existing.union(value)
    setattr(node.attributes, attr, value)


def add_suffix_mapping(env, source, target=None):
    """Map the suffix source to target"""
    if isinstance(source, str):
        if target not in AVAILABLE_ROLES:
            raise Exception(
                "target {} is not a known role. Available roles are {}"
                .format(target, AVAILABLE_ROLES)
            )
        env[SUFFIX_MAP][env.subst(source)] = target

    if not isinstance(source, dict):
        raise Exception('source must be a dictionary or a string')

    for _, mapping in source.items():
        for role in mapping.default_roles:
            if role not in AVAILABLE_ROLES:
                raise Exception(
                    "target {} is not a known role. Available roles are {}"
                    .format(target, AVAILABLE_ROLES)
                )

    env[SUFFIX_MAP].update({env.subst(key): value for key, value in source.items()})

def suffix_mapping(env, source=False, target=False, **kwargs):
    """Generate a SuffixMap object from source and target."""
    return SuffixMap(
        directory=source if source else kwargs.get("directory"),
        default_roles=target if target else kwargs.get("default_roles"),
    )

def _aib_debugdir(source, target, env, for_signature):
    for s in source:
        # TODO: Dry this with auto_install_emitter
        # TODO: We shouldn't need to reach into the attributes of the debug tool like this.
        origin = getattr(s.attributes, "debug_file_for", None)
        oentry = env.Entry(origin)
        osuf = oentry.get_suffix()
        return env[SUFFIX_MAP].get(osuf)[0]

def exists(_env):
    """Always activate this tool."""
    return True

def generate(env):  # pylint: disable=too-many-statements
    """Generate the auto install builders."""
    bld = SCons.Builder.Builder(action = tarball_builder)
    env.Append(BUILDERS = {'TarBall': bld})

    env["PREFIX_BIN_DIR"] = "$INSTALL_DIR/bin"
    env["PREFIX_LIB_DIR"] = "$INSTALL_DIR/lib"
    env["PREFIX_DOC_DIR"] = "$INSTALL_DIR/share/doc"
    env["PREFIX_INCLUDE_DIR"] = "$INSTALL_DIR/include"
    env["PREFIX_DEBUG_DIR"] = _aib_debugdir
    env[SUFFIX_MAP] = {}
    env[ALIAS_MAP] = defaultdict(dict)
    env[ROLE_DEPENDENCIES] = {
        "debug": [
            "runtime",
        ],
        "dev": [
            "runtime",
            "common",
        ],
        "meta": [
            "dev",
            "runtime",
            "common",
            "debug",
        ],
        "runtime": [
            "common",
        ],
    }

    env.AddMethod(suffix_mapping, "SuffixMap")
    env.AddMethod(add_suffix_mapping, "AddSuffixMapping")
    env.AddMethod(auto_install, "AutoInstall")
    env.AddMethod(finalize_install_dependencies, "FinalizeInstallDependencies")
    env.Tool("install")

    for builder in ["Program", "SharedLibrary", "LoadableModule", "StaticLibrary"]:
        builder = env["BUILDERS"][builder]
        base_emitter = builder.emitter
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
