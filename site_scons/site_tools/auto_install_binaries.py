# TODO: Distfiles and equivalent for the dist target
# TODO: move keep_targetinfo to tag_install
# TODO: Add test tag automatically for unit tests, etc.
# TODO: Test tag still leaves things in the runtime component
# TODO: Debug info locations should follow associated binary
# TODO: How should debug info work for tests?
# TODO: Handle chmod state
# TODO: tarfile generation
# TODO: library dependency chaining for windows dynamic builds, static dev packages
# TODO: Injectible component dependencies (jscore -> resmoke, etc.)
# TODO: Installing resmoke and configurations
# TODO: package decomposition
# TODO: Install/package target help text
# TODO: destdir vs prefix (what about --install-sandbox?)
# TODO: implement sdk_headers
# TODO: Namedtuple for alias_map

import itertools
from collections import defaultdict

import SCons


def exists(env):
    return True


def generate(env):

    role_tags = set([
        'common',
        'debug',
        'dev',
        'meta',
        'runtime',
    ])

    role_dependencies = {
        'dev' : ['runtime', 'common'],
        'meta' : ['dev', 'runtime', 'common', 'debug'],
        'runtime' : ['common'],
    }

    env.Tool('install')

    # TODO: These probably need to be patterns of some sort, not just suffixes.
    suffix_map = {
        env.subst('$PROGSUFFIX') : ('bin', ['runtime',]),
        env.subst('$LIBSUFFIX') : ('lib', ['dev',]),

        # TODO: Debug symbols?
        '.dll' :   ('bin', ['runtime',]),
        # TODO: The runtime libs should be in runtime, the dev symlinks in dev
        '.dylib' : ('lib', ['runtime', 'dev',]),
        '.so' :    ('lib', ['runtime', 'dev',]),
        # TODO: These 'lib' answers are incorrect. The location for the debug info
        # should be the same as the target itself, which might be bin or lib. We need
        # a solution for that. When that is fixed, add 'Program' back into the list
        # of separate debug targets in the separate_debug.py tool.
        '.dSYM': ('lib',),
        '.debug': ('lib',),
        '.lib': ('lib',),
    }

    alias_map = defaultdict(dict)

    def auto_install(env, target, source, **kwargs):
        prefixDir = env.Dir('$INSTALL_DIR')

        actions = []
        targetDir = prefixDir.Dir(target)

        actions = SCons.Script.Install(
            target=targetDir,
            source=source,
        )
        for s in map(env.Entry, env.Flatten(source)):
            setattr(s.attributes, "aib_install_actions", actions)

        # Get the tags. If no tags were set, or a non-falsish thing
        # was set then interpret that as a request for normal
        # tagging. Auto include the 'all' tag, and generate
        # aliases. If the user explicitly set the INSTALL_ALIAS to
        # something falsy, interpret that as meaning no tags at all,
        # so that we have a way to exempt targets from auto
        # installation.
        tags = kwargs.get('INSTALL_ALIAS', None)
        normalized_tags = list()
        if tags is None or tags:
            tags = set(tags or [])
            for tag in tags:
                if not isinstance(tag, tuple):
                    normalized_tags.append(tag)
                    continue
                normalized_tags.append('-'.join(tag))
                normalized_tags.append(tag[-1])
        tags = set(normalized_tags)

        applied_role_tags = role_tags.intersection(tags)
        applied_component_tags = tags - applied_role_tags

        # The 'all' tag is implicitly attached as a component, and the
        # 'meta' tag is implicitly attached as a role.
        applied_role_tags.add("meta")
        applied_component_tags.add("all")

        for component_tag, role_tag in itertools.product(applied_component_tags, applied_role_tags):
            alias_name = 'install-' + component_tag
            alias_name = alias_name + ("" if role_tag == "runtime" else "-" + role_tag)
            prealias_name = 'pre' + alias_name
            alias = env.Alias(alias_name, actions)
            prealias = env.Alias(prealias_name, source)
            alias_map[component_tag][role_tag] = (alias_name, alias, prealias_name, prealias)

        return actions

    env.AddMethod(auto_install, 'AutoInstall')

    def finalize_install_dependencies(env):
        common_rolemap = alias_map.get('common', None)
        default_rolemap = alias_map.get('default', None)

        if default_rolemap and 'runtime' in default_rolemap:
            env.Alias('install', 'install-default')
            env.Default('install')

        for component, rolemap in alias_map.iteritems():
            for role, info in rolemap.iteritems():

                if common_rolemap and component != 'common' and role in common_rolemap:
                    env.Depends(info[1], common_rolemap[role][1])
                    env.Depends(info[3], common_rolemap[role][3])

                for dependency in role_dependencies.get(role, []):
                    dependency_info = rolemap.get(dependency, [])
                    if dependency_info:
                        env.Depends(info[1], dependency_info[1])
                        env.Depends(info[3], dependency_info[3])

        installedFiles = env.FindInstalledFiles()
        env.NoCache(installedFiles)

    env.AddMethod(finalize_install_dependencies, "FinalizeInstallDependencies")

    def auto_install_emitter(target, source, env):
        for t in target:
            tentry = env.Entry(t)
            # We want to make sure that the executor information stays
            # persisted for this node after it is built so that we can
            # access it in our install emitter below.
            tentry.attributes.keep_targetinfo = 1
            tsuf = tentry.get_suffix()
            auto_install_location = suffix_map.get(tsuf)
            if auto_install_location:
                tentry_install_tags = env.get('INSTALL_ALIAS', [])
                tentry_install_tags.extend(auto_install_location[1])
                setattr(tentry.attributes, 'INSTALL_ALIAS', tentry_install_tags)
                install = env.AutoInstall(auto_install_location[0], tentry, INSTALL_ALIAS=tentry_install_tags)
        return (target, source)

    def add_emitter(builder):
        base_emitter = builder.emitter
        new_emitter = SCons.Builder.ListEmitter([base_emitter, auto_install_emitter])
        builder.emitter = new_emitter

    target_builders = ['Program', 'SharedLibrary', 'LoadableModule', 'StaticLibrary']
    for builder in target_builders:
        builder = env['BUILDERS'][builder]
        add_emitter(builder)

    def scan_for_transitive_install(node, env, path=()):
        results = []
        install_sources = node.sources
        for install_source in install_sources:
            is_executor = install_source.get_executor()
            if not is_executor:
                continue
            is_targets = is_executor.get_all_targets()
            for is_target in (is_targets or []):
                grandchildren = is_target.children()
                for grandchild in grandchildren:
                    actions = getattr(grandchild.attributes, "aib_install_actions", None)
                    if actions:
                        results.extend(actions)
        results = sorted(results, key=lambda t: str(t))
        return results

    from SCons.Tool import install
    base_install_builder = install.BaseInstallBuilder
    assert base_install_builder.target_scanner is None

    base_install_builder.target_scanner = SCons.Scanner.Scanner(
        function=scan_for_transitive_install,
        path_function=None,
    )
