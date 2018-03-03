import SCons

def exists(env):
    return True

def generate(env):

    env.Tool('install')

    suffix_map = {
        '.dylib' : 'lib',
        '.so' : 'lib',
    }

    def tag_install(env, target, source, **kwargs):
        prefixDir = env.Dir('$INSTALL_DIR')

        actions = []
        targetDir = prefixDir.Dir(target)

        actions = SCons.Script.Install(
            target=targetDir,
            source=source,
        )
        for s in map(env.Entry, env.Flatten(source)):
            setattr(s.attributes, "aib_install_actions", actions)

        tags = kwargs.get('INSTALL_ALIAS', [])
        if tags:
            env.Alias(tags, actions)

        return actions

    env.AddMethod(tag_install, 'Install')

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
                install = env.Install(auto_install_location, tentry, INSTALL_ALIAS=tentry_install_tags)
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
            is_targets = is_executor.get_all_targets()
            for is_target in is_targets:
                grandchildren = is_target.children()
                for grandchild in grandchildren:
                    actions = getattr(grandchild.attributes, "aib_install_actions", None)
                    if actions:
                        results.extend(actions)
        results = sorted(results, key=lambda t: str(t))
        return results

    from SCons.Tool import install
    base_install_builder = install.BaseInstallBuilder
    assert(base_install_builder.target_scanner == None)

    base_install_builder.target_scanner = SCons.Scanner.Scanner(
        function=scan_for_transitive_install,
        path_function=None,
    )
