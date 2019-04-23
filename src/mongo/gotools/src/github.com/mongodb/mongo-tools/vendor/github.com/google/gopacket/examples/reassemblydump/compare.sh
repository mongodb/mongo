#!/bin/bash

# Limitations: if the number extracted files in too big, finding identical
#              files might fail due to '*' in cmdline
#              This would require to split sha256sum symlinks in xx/yyyyy

usage()
{
        echo "Usage: $0 <file.pcap> <output-dir>"
        echo "Compares tcpreassembly against tcpflow"
        echo ""
        echo "$@"
        exit 1
}

debug() {
        return # comment me for debug
        echo "$@"
}

die()
{
        (
        echo "$@"
        echo
        ) >&2
        exit 1
}

rename()
{
        local path="$1"
        local filter="$2"
        find "$path" -type f -name "$filter" -print0 |
                while IFS= read -r -d $'\0' f; do
                        local sha256="$(sha256sum "$f" | cut -d ' ' -f 1)"
                        local target="$(dirname $f)/../sha256/$sha256"
                        debug "$target → $f"
                        mkdir -p "$(dirname "$target")" || return 1
                        if [ ! -f "$target" ]; then
                                ln -sr "$f" "$target" || return 1
                        fi
                done
        return $?
}

main()
{
        local src="$1"
        local out="$2"

        # TODO: make options
        local extra=""
        extra="$extra -debug"
        extra="$extra -cpuprofile "$out/gopacket/cpu.prof""
        extra="$extra -memprofile "$out/gopacket/mem.prof""

        [ ! -f "$src" ] && usage "Missing pcap"
        [ ! -d "$out" ] && ( mkdir "$out" || die "Failed to create $out" )

        mkdir -p "$out/gopacket/all" || die "Failed to create $out/gopacket/all"
        mkdir -p "$out/tcpflow/all" || die "Faield to create $out/tcpflow/all"

        echo " * Running go reassembly"
        time ./reassemblydump -r "$src" $debug -output "$out/gopacket/all" $extra -writeincomplete -ignorefsmerr -nooptcheck -allowmissinginit port 80 &> "$out/gopacket.txt" || die "Failed to run reassmbly. Check $out/gopacket.txt"
        echo " * Running tcpflow"
        time tcpflow -e http -r "$src" -o "$out/tcpflow/all" port 80 &> "$out/tcpflow.txt" || die "Failed to run tcpflow. Check $out/tcpflow.txt"

        echo " * Creating sha256sum symlinks for gopacket"
        rename "$out/gopacket/all" '*' || die "Failed to rename in $out/gopacket"
        echo " * Creating sha256sum symlinks for tcpflow"
        rename "$out/tcpflow/all" '*HTTPBODY*' || die "Failed to rename in $out/tcpflow"

        # Remove identical files
        echo " * Finding identical files"
        local nb=0
        mkdir -p "$out/gopacket/sha256-equal"
        mkdir -p "$out/tcpflow/sha256-equal"
        for f in "$out/gopacket/sha256/"*; do
                local f="$(basename "$f")"
                [ -f "$out/tcpflow/sha256/$f" ] && {
                        debug "    $f"
                        mv "$out/gopacket/sha256/$f" "$out/gopacket/sha256-equal"
                        mv "$out/tcpflow/sha256/$f"  "$out/tcpflow/sha256-equal"
                        nb=$((nb+1))
                }
        done
        echo "   →  found $nb files"

        echo " * Diffing {gopacket,tcpflow}/sha256"
        local rc=0
        for p in "gopacket" "tcpflow"; do
                local nb=$(ls -1 "$out/$p/sha256/" | wc -l)
                if [ $nb -ne 0 ]; then
                        rc=$((rc+1))
                        echo "   → $nb files in $out/$p/sha256"
                fi
        done
        return $rc
}

main "$@"
exit $?
