# Zydis disassembler

Zydis x64/x86 disassembler imported from github, see
https://github.com/zyantific/zydis and https://github.com/zyantific/zycore-c.

Zydis is MIT licensed code, see Zydis/LICENSE and Zycore/LICENSE.

Sources here were taken from the tag/revision of Zydis that is recorded in the
file imported-revision.txt.

The file hierarchy of Zydis+Zycore has been flattened and processed as described
in the script update.sh.

## Integrating new versions of Zydis

The procedure for pulling a new version is encoded in the script update.sh,
which is to be run from the parent directory of zydis, ie, from js/src/.  It
will create a new zydis directory and pull new files from github into it,
leaving the old zydis directory as zydis_old.

It's not a given that the script will work out of the box for new versions of
zydis or that the resulting files will build as-is.  Buyer beware.
