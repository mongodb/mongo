# vi: ft=tcl sw=2 ts=2 et

# https://bsonspec.org/spec.html

package require cmdline
include "BSON/_BSON.tcl-inc"

main_guard {
  while {![end]} {
    bson_document_detailed
  }
}

