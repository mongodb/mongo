# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

# TODO: Handle chmod state

from collections import defaultdict, namedtuple
from typing import List

import SCons
from SCons.Tool import install

ALIAS_MAP = "AIB_ALIAS_MAP"
BASE_COMPONENT = "AIB_BASE_COMPONENT"
BASE_ROLE = "AIB_BASE_ROLE"
COMPONENT = "AIB_COMPONENT"
REVERSE_COMPONENT_DEPENDENCIES = "AIB_COMPONENTS_EXTRA"
DEFAULT_COMPONENT = "AIB_DEFAULT_COMPONENT"
INSTALLED_FILES = "AIB_INSTALLED_FILES"
META_COMPONENT = "AIB_META_COMPONENT"
META_ROLE = "AIB_META_ROLE"
ROLE = "AIB_ROLE"
ROLE_DECLARATIONS = "AIB_ROLE_DECLARATIONS"
SUFFIX_MAP = "AIB_SUFFIX_MAP"
TASKS = "AIB_TASKS"

SuffixMap = namedtuple(
    "SuffixMap",
    ["directory", "default_role"],
)


class RoleInfo:
    """A component/role union Node."""

    def __init__(self, component, role, files=None, dependencies=None):
        self.id = "{}-{}".format(component, role)
        self.component = component
        self.role = role
        if files is None:
            self.files = set()
        else:
            self.files = set(files)

        if dependencies is None:
            self.dependencies = set()
        else:
            self.dependencies = set(dependencies)

    def __str__(self):
        return "RoleInfo({})".format(self.id)

    def __repr__(self):
        return self.__str__()


class DeclaredRole:
    def __init__(self, name, dependencies=None, transitive=False, silent=False):
        self.name = name

        if dependencies is None:
            self.dependencies = set()
        else:
            self.dependencies = {dep for dep in dependencies if dep is not None}

        self.silent = silent


def declare_role(env, **kwargs):
    """Construct a new role declaration"""
    return DeclaredRole(**kwargs)


def declare_roles(env, roles, base_role=None, meta_role=None):
    """Given a list of role declarations, validate them and store them in the environment"""
    role_names = [role.name for role in roles]
    if len(role_names) != len(set(role_names)):
        raise Exception("Cannot declare duplicate roles")

    # Ensure that all roles named in dependency lists actually were
    # passed in as a role.
    for role in roles:
        for d in role.dependencies:
            if d not in role_names:
                raise Exception("Role dependency '{}' does not name a declared role".format(d))

    if isinstance(base_role, str):
        if base_role not in role_names:
            raise Exception(
                "A base_role argument was provided but it does not name a declared role")
    elif isinstance(base_role, DeclaredRole):
        if base_role not in roles:
            raise Exception("A base_role argument was provided but it is not a declared role")
    elif base_role is not None:
        raise Exception("The base_role argument must be a string name of a role or a role object")
    else:
        # Set it to something falsey
        base_role = str()

    if isinstance(meta_role, str):
        if meta_role not in role_names:
            raise Exception(
                "A meta_role argument was provided but it does not name a declared role")
    elif isinstance(meta_role, DeclaredRole):
        if meta_role not in roles:
            raise Exception("A meta_role argument was provided but it is not a declared role")
    elif meta_role is not None:
        raise Exception("The meta_role argument must be a string name of a role or a role object")
    else:
        # Set it to something falsy
        meta_role = str()

    silents = [role for role in roles if role.silent]
    if len(silents) > 1:
        raise Exception("No more than one role can be declared as silent")

    # If a base role was given, then add it as a dependency of every
    # role that isn't the base role (which would be circular).
    if base_role:
        for role in roles:
            if role.name != base_role:
                role.dependencies.add(base_role)

    # Become a dictionary, so we can look up roles easily.
    roles = {role.name: role for role in roles}

    # If a meta role was given, then add every role which isn't the
    # meta role as one of its dependencies.
    if meta_role:
        roles[meta_role].dependencies.update(r for r in roles.keys() if r != meta_role)

    # TODO: Check for DAG

    # TODO: What if base_role or meta_role is really None?
    env[BASE_ROLE] = base_role
    env[META_ROLE] = meta_role
    env[ROLE_DECLARATIONS] = roles


def generate_alias_name(env, component, role, task):
    """Generate a scons alias for the component and role combination"""
    return "{task}-{component}{role}".format(
        task=task,
        component=component,
        role="" if env[ROLE_DECLARATIONS][role].silent else "-" + role,
    )


def get_alias_map_entry(env, component, role):
    c_entry = env[ALIAS_MAP][component]

    try:
        return c_entry[role]
    except KeyError:
        r_entry = RoleInfo(component=component, role=role)
        c_entry[role] = r_entry

        declaration = env[ROLE_DECLARATIONS].get(role)
        for dep in declaration.dependencies:
            dep_entry = get_alias_map_entry(env, component, dep)
            r_entry.dependencies.add(dep_entry)

        meta_component = env.get(META_COMPONENT)
        if meta_component and component != meta_component:
            meta_c_entry = get_alias_map_entry(env, meta_component, role)
            meta_c_entry.dependencies.add(r_entry)

        base_component = env.get(BASE_COMPONENT)
        if base_component and component != base_component:
            base_c_entry = get_alias_map_entry(env, base_component, role)
            r_entry.dependencies.add(base_c_entry)

        meta_role = env.get(META_ROLE)
        if (meta_role and role != meta_role and meta_component and component != meta_component):
            meta_r_entry = get_alias_map_entry(env, component, meta_role)
            meta_c_r_entry = get_alias_map_entry(env, meta_component, meta_role)
            meta_c_r_entry.dependencies.add(meta_r_entry)

        return r_entry


def get_component(node):
    return getattr(node.attributes, COMPONENT, None)


def get_role(node):
    return getattr(node.attributes, ROLE, None)


def scan_for_transitive_install(node, env, _path):
    """Walk the children of node finding all installed dependencies of it."""
    component = get_component(node.sources[0])
    role = get_role(node.sources[0])
    if component is None:
        return []

    scanned = getattr(node.attributes, "AIB_SCANNED", None)
    if scanned is not None:
        return scanned

    # Access directly by keys because we don't want to accidentally
    # create a new entry via get_alias_map_entry and instead should
    # throw a KeyError if we got here without valid components and
    # roles
    alias_map = env[ALIAS_MAP]
    entry = alias_map[component][role]
    role_deps = env[ROLE_DECLARATIONS].get(role).dependencies
    results = set()

    # We have to explicitly look at the various BASE files here since it's not
    # guaranteed they'll be pulled in anywhere in our grandchildren but we need
    # to always depend upon them. For example if env.AutoInstall some file 'foo'
    # tagged as common base but it's never used as a source for the
    # AutoInstalled file we're looking at or the children of our children (and
    # so on) then 'foo' would never get scanned in here without this explicit
    # dependency adding.
    base_component = env.get(BASE_COMPONENT)
    if base_component and component != base_component:
        base_role_entry = alias_map[base_component][role]
        if base_role_entry.files:
            results.update(base_role_entry.files)

    base_role = env.get(BASE_ROLE)
    if base_role and role != base_role:
        component_base_entry = alias_map[component][base_role]
        if component_base_entry.files:
            results.update(component_base_entry.files)

    if (base_role and base_component and component != base_component and role != base_role):
        base_base_entry = alias_map[base_component][base_role]
        if base_base_entry.files:
            results.update(base_base_entry.files)

    installed_children = set(grandchild for child in node.children()
                             for direct_children in child.children()
                             for grandchild in direct_children.get_executor().get_all_targets()
                             if direct_children.get_executor() and grandchild.has_builder())

    for child in installed_children:
        auto_installed_files = get_auto_installed_files(env, child)
        if not auto_installed_files:
            continue

        child_role = get_role(child)
        if child_role == role or child_role in role_deps:
            child_component = get_component(child)
            child_entry = get_alias_map_entry(env, child_component, child_role)

            # This is where component inheritance happens. We need a default
            # component for everything so we can store it but if during
            # transitive scanning we see a child with the default component here
            # we will move that file to our component. This prevents
            # over-stepping the DAG bounds since the default component is likely
            # to be large and an explicitly tagged file is unlikely to depend on
            # everything in it.
            if child_component == env.get(DEFAULT_COMPONENT):
                setattr(node.attributes, COMPONENT, component)
                for f in auto_installed_files:
                    child_entry.files.discard(f)
                entry.files.update(auto_installed_files)
            elif component != child_component:
                entry.dependencies.add(child_entry)

            results.update(auto_installed_files)

    # Produce deterministic output for caching purposes
    results = sorted(results, key=str)
    setattr(node.attributes, "AIB_SCANNED", results)

    return results


def scan_for_transitive_install_pseudobuilder(env, node):
    return scan_for_transitive_install(node, env, None)


def tag_components(env, target, **kwargs):
    """Create component and role dependency objects"""
    target = env.Flatten([target])
    component = kwargs.get(COMPONENT)
    role = kwargs.get(ROLE)
    if component is not None and (not isinstance(component, str) or " " in component):
        raise Exception("AIB_COMPONENT must be a string and contain no whitespace.")

    if component is None:
        raise Exception("AIB_COMPONENT must be provided; untagged targets: {}".format(
            [t.path for t in target]))

    if role is None:
        raise Exception("AIB_ROLE was not provided.")

    for t in target:
        t.attributes.keep_targetinfo = 1
        setattr(t.attributes, COMPONENT, component)
        setattr(t.attributes, ROLE, role)

    entry = get_alias_map_entry(env, component, role)

    # We cannot wire back dependencies to any combination of meta role, meta
    # component or base component. These cause dependency cycles because
    # get_alias_map_entry will do that wiring for us then we will try to
    # map them back on themselves in our loop.
    if (component != env.get(BASE_COMPONENT) and role != env.get(META_ROLE)
            and component != env.get(META_COMPONENT)):
        for component in kwargs.get(REVERSE_COMPONENT_DEPENDENCIES, []):
            component_dep = get_alias_map_entry(env, component, role)
            component_dep.dependencies.add(entry)

    return entry


def auto_install_task(env, component, role):
    """Auto install task."""
    entry = get_alias_map_entry(env, component, role)
    return list(entry.files)


def auto_install_pseudobuilder(env, target, source, **kwargs):
    """Auto install pseudo-builder."""
    source = env.Flatten([source])
    source = [env.File(s) for s in source]
    entry = env.TagComponents(source, **kwargs)

    installed_files = []
    for s in source:

        target_for_source = target

        if not target_for_source:

            # AIB currently uses file suffixes to do mapping. However, sometimes we need
            # to do the mapping based on a different suffix. This is used for things like
            # dSYM files, where we really just want to describe where .dSYM bundles should
            # be placed, but need to actually handle the substructure. Currently, this is
            # only used by separate_debug.py.
            #
            # TODO: Find a way to do this without the tools needing to coordinate.
            suffix = getattr(s.attributes, "aib_effective_suffix", s.get_suffix())
            auto_install_mapping = env[SUFFIX_MAP].get(suffix)

            if not auto_install_mapping:
                raise Exception("No target provided and no auto install mapping found for:", str(s))

            target_for_source = auto_install_mapping.directory

        # We've already auto installed this file and it may have belonged to a
        # different role since it wouldn't get retagged above. So we just skip
        # this files since SCons will already wire the dependency since s is a
        # source and so the file will get installed. A common error here is
        # adding debug files to the runtime component file if we do not skip
        # this.
        existing_installed_files = get_auto_installed_files(env, s)
        if existing_installed_files:
            continue

        # We must do an early subst here so that the _aib_debugdir
        # generator has a chance to run while seeing 'source'. We need
        # to do two substs here.  The first is to expand an variables
        # in `target_for_source` while we can see `source`. This is
        # needed for things like _aib_debugdir. Then, we need to do a
        # second subst to expand DESTDIR, interpolating
        # `target_for_source` in as $TARGET. Yes, this is confusing.
        target_for_source = env.subst(target_for_source, source=s)
        target_for_source = env.Dir(env.subst('$DESTDIR/$TARGET', target=target_for_source))

        aib_additional_directory = getattr(s.attributes, "aib_additional_directory", None)
        if aib_additional_directory is not None:
            target_for_source = env.Dir(aib_additional_directory, directory=target_for_source)

        new_installed_files = env.Install(target=target_for_source, source=s)
        setattr(s.attributes, INSTALLED_FILES, new_installed_files)

        installed_files.extend(new_installed_files)

    entry.files.update(installed_files)
    return installed_files


def finalize_install_dependencies(env):
    """Generates task aliases and wires install dependencies."""

    # Wire up component dependencies and generate task aliases
    for task, func in env[TASKS].items():
        generate_dependent_aliases = True

        # The task map is a map of string task names (i.e. "install" by default)
        # to either a tuple or function. If it's a function we assume that we
        # generate dependent aliases for that task, otherwise if it's a tuple we
        # deconstruct it here to get the function (the first element) and a
        # boolean indicating whether or not to generate dependent aliases for
        # that task. For example the "archive" task added by the auto_archive
        # tool disables them because tarballs do not track dependencies so you
        # do not want archive-foo to build archive-bar as well if foo depends on
        # bar.
        if isinstance(func, tuple):
            func, generate_dependent_aliases = func

        for component, rolemap in env[ALIAS_MAP].items():
            for role, info in rolemap.items():
                alias_name = generate_alias_name(env, component, role, task)
                alias = env.Alias(alias_name, func(env, component, role))
                if generate_dependent_aliases:
                    dependent_aliases = env.Flatten([
                        env.Alias(generate_alias_name(env, d.component, d.role, task))
                        for d in info.dependencies
                    ])
                    env.Alias(alias, dependent_aliases)


def auto_install_emitter(target, source, env):
    """When attached to a builder adds an appropriate AutoInstall to that Builder."""

    for t in target:
        if isinstance(t, str):
            t = env.File(t)

        if env.get("AIB_IGNORE", False):
            continue

        # There is no API for determining if an Entry is operating in
        # a SConf context. We obviously do not want to auto tag, and
        # install conftest Programs. So we filter them out the only
        # way available to us.
        #
        # We're working with upstream to expose this information.
        if "conftest" in str(t):
            continue

        # Get the suffix, unless overridden
        suffix = getattr(t.attributes, "aib_effective_suffix", t.get_suffix())
        auto_install_mapping = env[SUFFIX_MAP].get(suffix)

        if auto_install_mapping is not None:
            env.AutoInstall(
                auto_install_mapping.directory,
                t,
                AIB_COMPONENT=env.get(COMPONENT, env.get(DEFAULT_COMPONENT, None)),
                AIB_ROLE=env.get(ROLE, auto_install_mapping.default_role),
                AIB_COMPONENTS_EXTRA=env.get(REVERSE_COMPONENT_DEPENDENCIES, []),
            )

    return (target, source)


def add_suffix_mapping(env, suffix, role=None):
    """Map suffix to role"""
    if isinstance(suffix, str):
        if role not in env[ROLE_DECLARATIONS]:
            raise Exception("target {} is not a known role available roles are {}".format(
                role, env[ROLE_DECLARATIONS].keys()))
        env[SUFFIX_MAP][env.subst(suffix)] = role

    if not isinstance(suffix, dict):
        raise Exception("source must be a dictionary or a string")

    for _, mapping in suffix.items():
        role = mapping.default_role
        if role not in env[ROLE_DECLARATIONS]:
            raise Exception("target {} is not a known role. Available roles are {}".format(
                target, env[ROLE_DECLARATIONS].keys()))

    env[SUFFIX_MAP].update({env.subst(key): value for key, value in suffix.items()})


def suffix_mapping(env, directory="", default_role=False):
    """Generate a SuffixMap object from source and target."""
    return SuffixMap(directory=directory, default_role=default_role)


def get_auto_installed_files(env, node):
    return getattr(node.attributes, INSTALLED_FILES, [])


def list_components(env, **kwargs):
    """List registered components for env."""
    print("Known AIB components:")
    for key in env[ALIAS_MAP]:
        print("\t", key)


def list_hierarchical_aib_recursive(mapping, counter=0):
    if counter == 0:
        print("  " * counter, mapping.id)
    counter += 1
    for dep in mapping.dependencies:
        print("  " * counter, dep.id)
        list_hierarchical_aib_targets(dep, counter=counter)


def list_hierarchical_aib_targets(dag_mode=False):
    def target_lister(env, **kwargs):
        if dag_mode:
            installed_files = set(env.FindInstalledFiles())
            for f in installed_files:
                scan_for_transitive_install(f, env, None)

        mapping = env[ALIAS_MAP][env[META_COMPONENT]][env[META_ROLE]]
        list_hierarchical_aib_recursive(mapping)

    return target_lister


def list_recursive(mapping) -> List[str]:
    items = set()
    items.add(mapping.id)
    for dep in mapping.dependencies:
        items |= list_recursive(dep)
    return items


def list_targets():
    def target_lister(env, **kwargs):
        mapping = env[ALIAS_MAP][env[META_COMPONENT]][env[META_ROLE]]
        tasks = sorted(list(env[TASKS].keys()))
        roles = sorted(list(env[ROLE_DECLARATIONS].keys()))
        targets_with_role = list(list_recursive(mapping)) + [mapping.id]
        targets: List[str] = []
        for target_role in targets_with_role:
            # Does this target_role end with one of our speicifed roles
            matching_roles = list(filter(target_role.endswith, [f"-{role}" for role in roles]))
            assert len(matching_roles) == 1

            targets.append(target_role[:-len(matching_roles[0])])

        # dedup and sort targets
        targets = sorted(list(set(targets)))
        print(
            "The following are AIB targets. Note that runtime role is implied if not specified. For example, install-mongod"
        )
        tasks_str = ','.join(tasks)
        print(f"TASK={{{tasks_str}}}")
        roles_str = ','.join(roles)
        print(f"ROLE={{{roles_str}}}")
        for target in targets:
            print(f"  TASK-{target}-ROLE")

    return target_lister


def get_role_declaration(env, role):
    return env[ROLE_DECLARATIONS][role]


def exists(_env):
    """Always activate this tool."""
    return True


def generate(env):
    """Generate the auto install builders."""
    env["AUTO_INSTALL_ENABLED"] = True

    # Matches the autoconf documentation:
    # https://www.gnu.org/prep/standards/html_node/Directory-Variables.html
    env["DESTDIR"] = env.Dir(env.get("DESTDIR", "#install"))
    env["PREFIX"] = env.get("PREFIX", ".")
    env["PREFIX_BINDIR"] = env.get("PREFIX_BINDIR", "$PREFIX/bin")
    env["PREFIX_LIBDIR"] = env.get("PREFIX_LIBDIR", "$PREFIX/lib")
    env["PREFIX_SHAREDIR"] = env.get("PREFIX_SHAREDIR", "$PREFIX/share")
    env["PREFIX_DOCDIR"] = env.get("PREFIX_DOCDIR", "$PREFIX_SHAREDIR/doc")
    env["PREFIX_INCLUDEDIR"] = env.get("PREFIX_INCLUDEDIR", "$PREFIX/include")
    env[SUFFIX_MAP] = {}
    env[ALIAS_MAP] = defaultdict(dict)

    env.AppendUnique(AIB_TASKS={
        "install": auto_install_task,
    })

    env.AddMethod(
        scan_for_transitive_install_pseudobuilder,
        "GetTransitivelyInstalledFiles",
    )
    env.AddMethod(get_role_declaration, "GetRoleDeclaration")
    env.AddMethod(get_auto_installed_files, "GetAutoInstalledFiles")
    env.AddMethod(tag_components, "TagComponents")
    env.AddMethod(auto_install_pseudobuilder, "AutoInstall")
    env.AddMethod(add_suffix_mapping, "AddSuffixMapping")
    env.AddMethod(declare_role, "Role")
    env.AddMethod(declare_roles, "DeclareRoles")
    env.AddMethod(finalize_install_dependencies, "FinalizeInstallDependencies")
    env.AddMethod(suffix_mapping, "SuffixMap")
    env.Tool("install")

    # TODO: we should probably expose these as PseudoBuilders and let
    # users define their own aliases for them.
    env.Alias("list-aib-components", [], [list_components])
    env.AlwaysBuild("list-aib-components")

    env.Alias("list-hierarchical-aib-targets", [], [list_hierarchical_aib_targets(dag_mode=False)])
    env.AlwaysBuild("list-hierarchical-aib-targets")

    env.Alias("list-hierarchical-aib-dag", [], [list_hierarchical_aib_targets(dag_mode=True)])
    env.AlwaysBuild("list-hierarchical-aib-dag")

    env.Alias("list-targets", [], [list_targets()])
    env.AlwaysBuild("list-targets")

    for builder in ["Program", "SharedLibrary", "LoadableModule", "StaticLibrary"]:
        builder = env["BUILDERS"][builder]
        base_emitter = builder.emitter
        # TODO: investigate if using a ListEmitter here can cause
        # problems if AIB is not loaded last
        new_emitter = SCons.Builder.ListEmitter([base_emitter, auto_install_emitter])
        builder.emitter = new_emitter

    base_install_builder = install.BaseInstallBuilder
    assert base_install_builder.target_scanner is None

    base_install_builder.target_scanner = SCons.Scanner.Scanner(
        function=scan_for_transitive_install,
        path_function=None,
    )
