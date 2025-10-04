# vi: ft=tcl sw=2 ts=2 et

little_endian

include "WT/_Utils.tcl-inc"
include "WT/_Consts.tcl-inc"
include "WT/_Impl.tcl-inc"
include "WT/_intpack.tcl-inc"
include "WT/_wtpage.tcl-inc"

set allocsize 4096

main_guard {
  wt_page $allocsize
}

