#!/bin/bash
set -e

# This script uses the openssl command line tool to create CRLs.

OUTPUT_PATH="jstests/libs/"

crl() {
    CADB=$(mktemp -d)
    CA="jstests/libs/ca.pem"
    CONFIG="${CADB}/config"
    DEST="${OUTPUT_PATH}/$1"
    echo '01' > "$CADB/serial"
    touch "$CADB/index.txt" "$CADB/index.txt.attr"
    echo -e "[ ca ]\ndefault_ca	= CA_default\n" > "$CONFIG"
    echo -e "[ CA_default ]\ndatabase = ${CADB}/index.txt\n" >> "$CONFIG"
    echo -e "certificate = $CA\nprivate_key = $CA\ndefault_md = sha256" >> "$CONFIG"

    DAYS="3651"
    CRLDAYS="3650"
    if [ "$2" = "expired" ]; then
        DAYS="1"
        CRLDAYS="1"
    elif [ "$2" = "revoked" ]; then
        openssl ca -config "$CADB/config" -revoke "jstests/libs/client_revoked.pem"
    fi
    openssl ca -config "$CADB/config" -gencrl -out "$DEST" -md sha256 -days "$DAYS" -crldays "$CRLDAYS"
}
crl crl.pem empty
crl crl_expired.pem expired
crl crl_client_revoked.pem revoked
