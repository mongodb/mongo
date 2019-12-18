import SCons


def exists(env):
    return True


def generate(env):

    env.Tool('install')

    suffix_map = {
        env.subst('$PROGSUFFIX'): 'bin',
        '.dylib': 'lib',
        # TODO: These 'lib' answers are incorrect. The location for the debug info
        # should be the same as the target itself, which might be bin or lib. We need
        # a solution for that. When that is fixed, add 'Program' back into the list
        # of separate debug targets in the separate_debug.py tool.
        '.dSYM': 'lib',
        '.debug': 'lib',
        '.so': 'lib',
        '.dll': 'bin',
        '.lib': 'lib',
    }

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
        if tags is None or tags:
            tags = set(tags or [])
            tags.add('all')
            if 'default' in tags:
                tags.remove('default')
                env.Alias('install', actions)
                env.Default('install')
            env.Alias(['install-' + tag for tag in tags], actions)

        return actions

    env.AddMethod(auto_install, 'AutoInstall')

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
                setattr(tentry.attributes, 'INSTALL_ALIAS', tentry_install_tags)
                install = env.AutoInstall(auto_install_location, tentry,
                                          INSTALL_ALIAS=tentry_install_tags)
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
    assert (base_install_builder.target_scanner == None)

    base_install_builder.target_scanner = SCons.Scanner.Scanner(
        function=scan_for_transitive_install,
        path_function=None,
    )
