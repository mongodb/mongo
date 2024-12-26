# Using dist/modstat

dist/modstat is a tool to examine the modules contents and their relations.
It uses the same engine as the `s_access` script.

## Modularity info

Information about WT modules configuration is stored in the
`dist/modularity/wt_defs.py` file.
It contains module names and their aliases in the file system and in C sources.

The initial list of modules is filled in as a list of subdirectories of `src/`
with exception of a few directories that don't resemble modules.

The list of aliases is filled in manually as best guess.

## Usage

### Help

The `-h` option prints the help message.

```shell
# print help
$ dist/modstat -h
```

### Listing things

The `-m` option lists all modules and their aliases.
`-l` option lists all entities that match the given pattern.

```shell
# list all modules
$ dist/modstat -m

# list everything that belongs to the module 'txn'
$ dist/modstat -l txn | less

# list all member of the WT_CKPT struct
$ dist/modstat -l '(WT_CKPT).'

# list all entities named '__wt_evict'
$ dist/modstat -l __wt_evict
```

### Access query

When querying for access patterns, the `-f` option is used to specify the "from",
and the `-t` option is used to specify the "to" entity.

The `-r` option reverses the output so that the output is grouped by the "to" entity
instead of the "from" entity (by default).

The level of detail can be controlled with the `-d` option. By default, the
detail level is set to `mod` where only the module name is printed.
Other options are `file`, `defn` and `full` where the level of details increases.

The `full` detail level also supports the `--color` option to colorize the output.

The level of detail of "from" and "to" entities can be controlled individually
with the `--df` and `--dt` options.

```shell
# What does the 'txn' module access?
$ dist/modstat -f txn | less

# Who accesses the 'txn' module?
$ dist/modstat -t txn | less
$ dist/modstat -t txn -r | less  # reverse the output

# Who accesses fields of the 'WT_CKPT' struct (reverse output_?
$ dist/modstat -t '(WT_CKPT).' -r
# ... and include self and unmodular entities
$ dist/modstat -t '(WT_CKPT).' -r --self --unmod
# ... and include source's file names
$ dist/modstat -t '(WT_CKPT).' -r --self --unmod --df file
# ... and include source's definition location in the file
$ dist/modstat -t '(WT_CKPT).' -r --self --unmod --df defn
# ... and include source's call location in the file
$ dist/modstat -t '(WT_CKPT).' -r --self --unmod --df full
# ... and colorize the output and use less with -R option
$ dist/modstat -t '(WT_CKPT).' -r --self --unmod --df full --color | less -R
```

