# vi: ft=tcl sw=2 ts=2 et

#package require crc32
#catch {package require nonexistentName}
#foreach p [lsort [package names]] { puts $p }

little_endian

requires 0 "41 D8 01 00"

include "WT/_Utils.tcl-inc"
include "WT/_Consts.tcl-inc"
include "WT/_Impl.tcl-inc"
include "WT/_intpack.tcl-inc"
include "WT/_wtpage.tcl-inc"

set allocsize 4096

main_guard {
  # __wt_block_desc
  ssection -collapsed header {
    sectionvalue "allocsize: [hx $allocsize],  num allocs: [expr {[len] / $allocsize}]"
    uint32 magic
    uint16 majorv
    uint16 minorv
    uint32 -hex checksum
    bytes 4 unused
    #bytes [expr {$allocsize - 16}] pad
    #entry "(calculated crc1)" [peek { goto 0 ::crc::crc32 -format 0x%X [bytes $allocsize] }]
    #entry "(calculated crc2)" [peek { goto 0 ::crc::crc32 -format 0x%X [string replace [bytes $allocsize] 8 11 "\0\0\0\0"] }]
    #entry "(calculated crc3)" [peek { goto 0 ::crc::crc32 -format 0x%X [bytes 1] }]
  }

  goto $allocsize
  while {![end]} {
    wt_page $allocsize
  }
}

