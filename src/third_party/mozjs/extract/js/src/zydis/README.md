# Zydis disassembler

Zydis x64/x86 disassembler imported from github, as described in Zydis/moz.yaml.
It is bound to a given revision of Zycore, through a git submodule.

The link between the two modules is maintained by hand: they are vendored
through different moz.yaml files, one in Zydis/moz.yaml and one in
Zycore/moz.yaml.
