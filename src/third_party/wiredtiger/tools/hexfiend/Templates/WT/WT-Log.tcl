# vi: ft=tcl sw=2 ts=2 et

#package require crc32

little_endian

#requires 0 "80 00 00 00 32 5D AE F0 00 00 00 00 00 00 00 00 64 10 10 00"
requires 16 "64 10 10 00"

include "WT/_Utils.tcl-inc"
include "WT/_Consts.tcl-inc"
include "WT/_Impl.tcl-inc"
include "WT/_intpack.tcl-inc"

# __wt_log_record , __wt_log_desc @ 0x10

proc log_record {} {
  globals WT_LOG_*
  ssection __wt_log_record {
    #uint32 len         ;# Record length including hdr
    xentry -var len { uint32 }
    uint32 -hex checksum
    xentry flags { format_bits [uint16] $WT_LOG_RECORD_f }
    bytes 2 unused
    uint32 mem_len     ;# Uncompressed len if needed
    # uint8 record[0]
  }
  return $len
}

proc log_desc {} {
  globals WT_LOG_*
  ssection record:__wt_log_desc {
    uint32 -hex log_magic  ; # WT_LOG_MAGIC = 0x101064u
    uint16 version  ; # = WT_LOG_VERSION = 5
    uint16 unused
    xentry log_size { dx [uint64] }
  }
}

proc read_record {name} {
  globals WT_LOG* WT_TXN_*
  ssection -collapsed $name {
    # __log_open_verify
    # __txn_printlog
    set _start [pos]
    set len [log_record]
    set desc "\[[xd $len]\]"
    if {$len < 16} { goto [len]; return 0 }
    ssection record {
      xentry -varname rectype_fmt rectype {
        format_enum [set rectype [vuint]] $WT_LOGREC_n
      }
      set rectype_shortfmt [strcut $rectype_fmt WT_LOGREC_]
      sectionname "record:$rectype_shortfmt"

      # __txn_printlog
      nswitch $rectype {
        $WT_LOGREC_SYSTEM {
          # __wt_log_recover_system -> __wt_logop_prev_lsn_unpack -> __wt_struct_unpack(fmt="IIII") -> __wt_struct_unpackv(fmt="IIII")
          xentry optype { format_enum [unpack_I] $WT_LOGOP_n }
          xentry size { unpack_I }
          xentry -var file { unpack_I }
          xentry -var offset { unpack_I }
          set desc "f:$file off:$offset"
        }
        $WT_LOGREC_CHECKPOINT {
          # __wt_txn_checkpoint_logread
          xentry optype { format_enum [unpack_I] $WT_LOGOP_n }
          xentry size { unpack_I }
          xentry -var file { unpack_I }
          xentry -var offset { unpack_I }
          xentry -var nsnapshot { unpack_I }
          xentry -var snapshot { unpack_i }
          set desc "f:$file off:$offset nsn:$nsnapshot sn:$snapshot"
        }
        $WT_LOGREC_COMMIT {
          xentry txnid { unpack_I }
          # __txn_op_apply -> __wt_logop_read
          ssection op {
            set _opstart [pos]
            xentry -varname optype_fmt optype { format_enum [set optype [unpack_I]] $WT_LOGOP_n }
            set optype_fmt [strcut $optype_fmt WT_LOGOP_]
            set desc $optype_fmt
            sectionvalue $optype_fmt
            xentry -var opsize { unpack_I }
            # __txn_op_apply
            if {$optype & $WT_LOGOP_IGNORE} {
              entry "WT_LOGOP_IGNORE" ""
              set desc "WT_LOGOP_IGNORE | [format_enum [expr {$optype & ~$WT_LOGOP_IGNORE}] $WT_LOGOP_n]"
              sectionvalue $desc
              goto [expr {$_opstart + $opsize}]
            } else {
              # __txn_op_apply
              nswitch $optype {
                $WT_LOGOP_COL_MODIFY | $WT_LOGOP_COL_PUT {
                  # __txn_op_apply -> __wt_logop_col_modify_unpack
                  xentry fileid { unpack_I }
                  xentry recno { unpack_r }
                  #xentry value { unpack_U }
                  xentry value { unpack_u_ascii [remaining $_opstart $opsize] }
                }
                $WT_LOGOP_COL_REMOVE {
                  # __txn_op_apply
                  xentry fileid { unpack_I }
                  xentry recno { unpack_r }
                }
                $WT_LOGOP_COL_TRUNCATE {
                  # __txn_op_apply
                  xentry fileid { unpack_I }
                  xentry start_recno { unpack_r }
                  xentry stop_recno { unpack_r }
                }
                $WT_LOGOP_ROW_PUT | $WT_LOGOP_ROW_MODIFY {
                  # __txn_op_apply -> __wt_logop_row_put_unpack
                  xentry fileid { unpack_I }
                  #xentry key { unpack_U_ascii }
                  set key [struct_U_ascii key]
                  #xentry value { unpack_U_ascii }
                  #xentry value { unpack_u_ascii [expr {$len - ([pos] - $_start)}] }
                  # last 'u' goes as 'U' with the remaining size
                  xentry value { unpack_u_ascii [remaining $_opstart $opsize] }
                  sectionvalue "$optype_fmt $key"
                  set desc "$desc $key"
                }
                $WT_LOGOP_ROW_REMOVE {
                  # __txn_op_apply -> __wt_logop_row_put_unpack
                  xentry fileid { unpack_I }
                  #xentry key { unpack_U_ascii }
                  #set key [struct_U_ascii key]
                  # last 'u' goes as 'U' with the remaining size
                  xentry key { unpack_u_ascii [remaining $_opstart $opsize] }
                  sectionvalue "$optype_fmt $key"
                  set desc "$desc $key"
                }
                $WT_LOGOP_ROW_TRUNCATE {
                  # __txn_op_apply -> __wt_logop_row_truncate_unpack
                  xentry fileid { unpack_I }
                  set start_key [struct_U_ascii key]
                  set stop_key [struct_U_ascii key]
                  set mode_fmt [valev mode { format_enum [unpack_I] $WT_TXN_TRUNC_MODE }]
                  sectionvalue "$optype_fmt [strcut $mode_fmt WT_TXN_TRUNC_] $start_key $stop_key"
                  set desc "$desc $start_key $stop_key"
                }
                $WT_LOGOP_TXN_TIMESTAMP {
                  # __txn_op_apply -> __wt_logop_txn_timestamp_unpack
                  xentry -var t_sec { unpack_Q }
                  xentry -var t_nsec { unpack_Q }
                  xentry -var commit { unpack_Q }
                  xentry -var durable { unpack_Q }
                  xentry -var first_commit { unpack_Q }
                  xentry -var prepare { unpack_Q }
                  xentry -var read { unpack_Q }
                  sectionvalue "$optype_fmt s:$t_sec ns:$t_nsec c:$commit d:$durable f:$first_commit p:$prepare r:$read"
                }
                default {}
              }
              #pad $_opstart $opsize
              gotoend $_opstart $opsize
            }
            gotoend $_opstart $opsize
          }
        }
        $WT_LOGREC_FILE_SYNC {
          xentry -var fileid { unpack_I }
          xentry -var start { unpack_i }
          set desc "f:$fileid start:$start"
        }
        $WT_LOGREC_MESSAGE {
          xentry -var msg { unpack_S_ascii [remaining $_opstart $opsize] }
          sectionvalue $msg
          set desc $msg
        }
        default {}
      }
    }
    sectionname "$name:$rectype_shortfmt"
    sectionvalue "$desc"

    #pad $_start $len
    gotoend $_start $len
  }
  return 1
}

main_guard2 {
  ssection -collapsed header {
    set _start [pos]
    set len [log_record]
    log_desc
    pad $_start $len
  }

  read_record system_record

  while {![end]} {
    read_record record
  }
}

