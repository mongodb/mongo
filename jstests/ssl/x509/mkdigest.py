#!/usr/bin/env python3
"""
This script calculates and writes out digests for x509 certificates/CRLs.
Invoke as `mkdigest.py <cert|crl> <sha256|sha1> <filename1> [filename2 ...]`
"""
import argparse
import OpenSSL
import cryptography.hazmat.primitives.hashes as hashes

DIGEST_NAME_TO_HASH = {'sha256': hashes.SHA256(), 'sha1': hashes.SHA1()}

def make_digest(filename, item_type, digest_type):
    """Calculate the given digest of the certificate/CRL passed in and write it out to <filename>.digest.<digest_type>"""
    assert item_type in {"cert", "crl"}
    assert digest_type in {"sha256", "sha1"}
    with open(filename, 'r') as f:
        data = f.read()

    if item_type == 'cert':
        cert = OpenSSL.crypto.load_certificate(OpenSSL.crypto.FILETYPE_PEM, data)
        rawdigest = cert.digest(digest_type)
        digest = rawdigest.decode('utf8').replace(':', '')
    elif item_type == 'crl':
        crl = OpenSSL.crypto.load_crl(OpenSSL.crypto.FILETYPE_PEM, data)
        rawdigest = crl.to_cryptography().fingerprint(DIGEST_NAME_TO_HASH[digest_type])
        digest = rawdigest.hex().upper()

    with open(filename + '.digest.' + digest_type, 'w') as f:
        f.write(digest)

def main():
    parser = argparse.ArgumentParser(description='X509 Digest Generator')
    parser.add_argument('type', choices={"cert", "crl"}, help='Type of X509 object to generate digest for')
    parser.add_argument('digest', choices={"sha1", "sha256"}, help='Algorithm for digest')
    parser.add_argument('filename', nargs='+', help='Path of X509 file to generate digest for')
    args = parser.parse_args()

    for fname in args.filename:
        make_digest(fname, args.type, args.digest)

if __name__ == '__main__':
    main()
