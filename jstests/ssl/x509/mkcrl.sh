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

    VALIDITY_OPTIONS="-days 824 -crldays 823"
    if [ "$2" = "expired" ]; then
        # -enddate 010101000000Z = expires on 0:00:00, Jan 1, 2000. 
        # -crlsec 1 = valid for 1 second from now. 
        # i.e. this certificate will be completely invalid very soon.
        VALIDITY_OPTIONS="-enddate 010101000000Z -crlsec 1"
    elif [ "$2" = "revoked" ]; then
        openssl ca -config "$CADB/config" -revoke "jstests/libs/client_revoked.pem"
    fi
    openssl ca -config "$CADB/config" -gencrl -out "$DEST" -md sha256 $VALIDITY_OPTIONS
    jstests/ssl/x509/mkdigest.py crl sha256 "$DEST"
    jstests/ssl/x509/mkdigest.py crl sha1 "$DEST"
}
crl crl.pem empty
crl crl_expired.pem expired
crl crl_client_revoked.pem revoked
