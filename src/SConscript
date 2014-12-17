# -*- mode: python; -*-
#
# This is the principle SConscript file, invoked by the SConstruct.  Its job is
# to delegate to any and all per-module SConscript files.

Import('env module_sconscripts')

env.SConscript(
    [
        # NOTE: We must do third_party first as it adds methods to the environment
        # that we need in the mongo sconscript
        'third_party/SConscript',
        'mongo/SConscript',
    ] + module_sconscripts
)
