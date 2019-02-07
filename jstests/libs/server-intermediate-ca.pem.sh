#!/bin/bash
# Create an intermediate signing authority and use it to sign a server certificate.
# Run this from the base directory of the server source.
set -ev

PREFIX="/C=US/ST=New York/L=New York City/O=MongoDB/OU=Kernel"
OPENSSL="/opt/mongodbtoolchain/v3/bin/openssl"

cd jstests/libs

# Build intermediate CA.
$OPENSSL req -new -subj "${PREFIX}/CN=Intermediate CA" \
             -keyout intermediate-ca.key -out intermediate-ca.csr \
             -nodes -batch -sha256 -newkey rsa:2048
$OPENSSL rsa -in intermediate-ca.key -out intermediate-ca.rsa
$OPENSSL x509 -in intermediate-ca.csr -out intermediate-ca.pem \
              -req -CA ca.pem -days 3650 -CAcreateserial

# Build leaf cert signed by intermediate CA.
$OPENSSL req -new -subj "${PREFIX}/CN=Server Via Intermediate" \
             -keyout server-intermediate-ca.key -out server-intermediate-ca.csr \
             -nodes -batch -sha256 -newkey rsa:2048
$OPENSSL rsa -in server-intermediate-ca.key -out server-intermediate-ca.rsa
$OPENSSL x509 -in server-intermediate-ca.csr -out server-intermediate-ca.pem \
              -req -CA intermediate-ca.pem -CAkey intermediate-ca.rsa \
              -days 3650 -CAcreateserial

# Create final bundle and cleanup.
cat server-intermediate-ca.rsa intermediate-ca.pem >> server-intermediate-ca.pem

rm ca.srl intermediate-ca.srl
rm server-intermediate-ca.key server-intermediate-ca.rsa server-intermediate-ca.csr
rm intermediate-ca.pem intermediate-ca.rsa intermediate-ca.key intermediate-ca.csr 
