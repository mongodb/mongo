#!/bin/bash
# Create a certificate with `CN=localhost` to satisfy CN matching,
# but override it with a SAN field which will not match.
set -ev

RDN="/C=US/ST=New York/L=New York City/O=MongoDB/OU=Kernel/CN=localhost"
OPENSSL="/opt/mongodbtoolchain/v3/bin/openssl"
FILE="jstests/ssl/libs/localhost-cn-with-san"

$OPENSSL req -new -subj "${RDN}" \
             -keyout "${FILE}.key" -out "${FILE}.csr" \
             -nodes -batch -sha256 -newkey rsa:2048
$OPENSSL rsa -in "${FILE}.key" -out "${FILE}.rsa"
$OPENSSL x509 -in "${FILE}.csr" -out "${FILE}.pem" -req -CA "jstests/libs/ca.pem" \
              -days 3650 -CAcreateserial \
              -extfile <(printf "subjectAltName=DNS:example.com")

# Create final bundle and cleanup.
cat "${FILE}.rsa" >> "${FILE}.pem"

rm jstests/libs/ca.srl
rm "${FILE}.key" "${FILE}.rsa" "${FILE}.csr"
