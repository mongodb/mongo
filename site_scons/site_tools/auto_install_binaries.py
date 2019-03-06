# TODO: Filter child chains by role (install bin/foo should not install lib/dep.debug)
# TODO: Add test tag automatically for unit tests, etc.
# TODO: Test tag still leaves things in the runtime component
# TODO: But meta doesn't depend on test! Should it though?
# TODO: How should debug info work for tests?
# TODO: destdir vs prefix (what about --install-sandbox?)
# TODO: Versioned libraries
# TODO: library dependency chaining for windows dynamic builds, static dev packages
# TODO: Injectible component dependencies (jscore -> resmoke, etc.)
# TODO: Distfiles and equivalent for the dist target
# TODO: Handle chmod state
# TODO: tarfile generation
# TODO: Installing resmoke and configurations
# TODO: package decomposition
# TODO: Install/package target help text
# TODO: implement sdk_headers
# TODO: Namedtuple for alias_map

import itertools
from collections import defaultdict

import SCons


def exists(env):
    return True


def generate(env):

    env['INSTALLDIR_BINDIR'] = '$INSTALL_DIR/bin'
    env['INSTALLDIR_LIBDIR'] = '$INSTALL_DIR/lib'
    env['INSTALLDIR_INCLUDEDIR'] = '$INSTALL_DIR/include'

    role_tags = set([
        'common',
        'debug',
        'dev',
        'meta',
        'runtime',
    ])

    role_dependencies = {
        'debug' : [
            'runtime',
        ],
        'dev' : [
            'runtime',
            'common',
        ],
        'meta' : [
            'dev',
            'runtime',
            'common',
            'debug',
        ],
        'runtime' : [
            'common',
        ],
    }

    env.Tool('install')

    # TODO: These probably need to be patterns of some sort, not just suffixes.
    # TODO: The runtime libs should be in runtime, the dev symlinks in dev
    suffix_map = {
        env.subst('$PROGSUFFIX') : (
            '$INSTALLDIR_BINDIR', [
                'runtime',
            ]
        ),

        env.subst('$LIBSUFFIX') : (
            '$INSTALLDIR_LIBDIR', [
                'dev',
            ]
        ),

        '.dll' : (
            '$INSTALLDIR_BINDIR', [
                'runtime',
            ]
        ),

        '.dylib' : (
            '$INSTALLDIR_LIBDIR', [
                'runtime',
                'dev',
            ]
        ),

        '.so' : (
            '$INSTALLDIR_LIBDIR', [
                'runtime',
                'dev',
            ]
        ),

        '.debug' : (
            '$INSTALLDIR_DEBUGDIR', [
                'debug',
            ]
        ),

        '.dSYM' : (
            '$INSTALLDIR_LIBDIR', [
                'runtime'
            ]
        ),

        '.lib' : (
            '$INSTALLDIR_LIBDIR', [
                'runtime'
            ]
        ),
    }

    def _aib_debugdir(source, target, env, for_signature):
        for s in source:
            # TODO: Dry this with auto_install_emitter
            # TODO: We shouldn't need to reach into the attributes of the debug tool like this.
            origin = getattr(s.attributes, "debug_file_for", None)
            oentry = env.Entry(origin)
            osuf = oentry.get_suffix()
            return suffix_map.get(osuf)[0]

    env['INSTALLDIR_DEBUGDIR'] = _aib_debugdir

    alias_map = defaultdict(dict)

    def auto_install(env, target, source, **kwargs):

        target = env.Dir(env.subst(target, source=source))

        # We want to make sure that the executor information stays
        # persisted for this node after it is built so that we can
        # access it in our install emitter below.
        source = map(env.Entry, env.Flatten([source]))
        for s in source:
            s.attributes.keep_targetinfo = 1

        actions = SCons.Script.Install(
            target=target,
            source=source,
        )

        for s in source:
            s.attributes.aib_install_actions = actions

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
            tsuf = tentry.get_suffix()
            auto_install_location = suffix_map.get(tsuf)
            if auto_install_location:
                tentry_install_tags = env.get('INSTALL_ALIAS', [])
                tentry_install_tags.extend(auto_install_location[1])
                setattr(tentry.attributes, 'INSTALL_ALIAS', tentry_install_tags)
                env.AutoInstall(auto_install_location[0], tentry, INSTALL_ALIAS=tentry_install_tags)
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
