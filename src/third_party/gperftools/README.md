

Don't make manual changes to files in the dist/ or platform/ directories.
Source code changes should be made in the mongodb-labs/gperftools repo.
Tweaks or reconfigurations of our third-party installation have to be
made in scripts/ or SConscript files, etc.

== contents ==

```
    src/third_party/
        gperftool-2.7/
            dist/                # 'make distdir' snapshot of mongodb-labs/gperftools
            platform/
                linux_x86_64/    # per platform directories
                    include/     # headers used by consumers of gperftools
                    internal/    # headers used in the compile of gperftools
                ...              # other platforms: $os_$arch
                windows_x86_64/  # special case that's always regenerated, and always the same way.
            scripts/
                import.sh        # regenerate the dist/ snapshot from git repo.
                host_config.sh   # generate platform headers dir for the host and windows_x86_64.
```

