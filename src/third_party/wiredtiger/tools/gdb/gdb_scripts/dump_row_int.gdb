# A helper script to dump row store internal page keys.
# Usage: dump_row_int_keys <page>

# Dump keys from a row store internal page.
# Has one argument which is the page.
define dump_row_int_keys
  set pagination off
  set $i = 0
  set $page = $arg0
  while $i < $page->u->intl->__index.entries
    set $ikey = $page->u->intl->__index->index[$i].key.ikey
    set $v = (uintptr_t)$ikey

    set $keyp = 0
    set $size = 0
    set $key_location = ""

    if $v & 0x1
      set $key_location = "DSK:"
# Relevant code snippets from WiredTiger we are trying to simulate.
#    #define WT_IK_FLAG 0x01
#    #define WT_IK_DECODE_KEY_LEN(v) ((v) >> 32)
#    #define WT_IK_DECODE_KEY_OFFSET(v) (((v)&0xFFFFFFFF) >> 1)
#    #define WT_PAGE_REF_OFFSET(page, o) ((void *)((uint8_t *)((page)->dsk) + (o)))
#      if (v & WT_IK_FLAG) {
#          *(void **)keyp = WT_PAGE_REF_OFFSET(page, WT_IK_DECODE_KEY_OFFSET(v));
#          *sizep = WT_IK_DECODE_KEY_LEN(v);
      set $key_offset = (($v) & 0xFFFFFFFF) >> 1
      set $keyp = ((void *)((uint8_t *)($page->dsk) + $key_offset))
      set $size = ($v) >> 32
    else
      set $key_location = "MEM:"
      set $keyp = ((void *)((uint8_t *)($ikey) + 8))
      set $size = ((WT_IKEY*)$ikey)->size
    end
    set $sz_pad = ""
    if $size < 10
      set $sz_pad = "  "
    else
      if $size < 100
        set $sz_pad = " "
      end
    end
    set $off_pad = ""
    if $i < 10
      set $off_pad = "   "
    else
      if $i < 100
        set $off_pad = "  "
      else
        if $i < 1000
          set $off_pad = " "
        end
      end
    end
    printf "%s%u:%s%s%u: ", $off_pad, $i, $key_location, $sz_pad, $size
    set $j = 0
    while $j < $size
      printf "%c", ((char*)$keyp)[$j]
      set $j = $j + 1
    end
    printf "\n"

    set $i = $i + 1   
  end
end
