#!/bin/bash
set -e

# This script uses the openssl command line tool to create CRLs.

OUTPUT_PATH="jstests/libs/"
CA_PEM_PATH="${OUTPUT_PATH}/ca.pem"
TRUSTED_CA_PEM_PATH="${OUTPUT_PATH}/trusted-ca.pem"

die () {
    [ $# -gt 0 ] && [ ! -z "$1" ] && echo "$1" >&2
    exit 1
}

# Usage:
#   crl <ca_pem_file> <output_crl_file> {empty|expired|revoked} [pem_file_to_revoke]
crl() {
    [ $# -lt 3 ] && die "Error: too few arguments"
    [ -z "$1" ] && die "Error: must supply a CA file"
    [ -z "$2" ] && die "Error: must supply an output filename"
    [ "$3" = "revoked" ] && [ -z "$4" ] && die "Error: must supply a certificate file to revoke"

    CADB=$(mktemp -d)
    CA="$1"
    CONFIG="${CADB}/config"
    DEST="${OUTPUT_PATH}/$2"
    echo '01' > "$CADB/serial"
    touch "$CADB/index.txt" "$CADB/index.txt.attr"
    echo -e "[ ca ]\ndefault_ca	= CA_default\n" > "$CONFIG"
    echo -e "[ CA_default ]\ndatabase = ${CADB}/index.txt\n" >> "$CONFIG"
    echo -e "certificate = $CA\nprivate_key = $CA\ndefault_md = sha256" >> "$CONFIG"

    VALIDITY_OPTIONS="-days 824 -crldays 823"
    if [ "$3" = "expired" ]; then
        # -enddate 010101000000Z = expires on 0:00:00, Jan 1, 2000.
        # -crlsec 1 = valid for 1 second from now.
        # i.e. this certificate will be completely invalid very soon.
        VALIDITY_OPTIONS="-enddate 010101000000Z -crlsec 1"
    elif [ "$3" = "revoked" ]; then
        openssl ca -config "$CADB/config" -revoke "$4"
    fi
    openssl ca -config "$CADB/config" -gencrl -out "$DEST" -md sha256 $VALIDITY_OPTIONS
    jstests/ssl/x509/mkdigest.py crl sha256 "$DEST"
    jstests/ssl/x509/mkdigest.py crl sha1 "$DEST"
}

crl $CA_PEM_PATH crl.pem empty
crl $CA_PEM_PATH crl_expired.pem expired
crl $CA_PEM_PATH crl_client_revoked.pem revoked "jstests/libs/client_revoked.pem"
crl $CA_PEM_PATH crl_intermediate_ca_B_revoked.pem revoked "jstests/libs/intermediate-ca-B.pem"
crl $TRUSTED_CA_PEM_PATH crl_from_trusted_ca.pem empty
crl "jstests/libs/intermediate-ca-B.pem" crl_from_intermediate_ca_B.pem empty
