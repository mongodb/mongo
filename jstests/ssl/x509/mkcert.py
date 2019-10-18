#!/usr/bin/env python3
"""
This script (re)generates x509 certificates from a definition file.
Invoke as `python3 jstests/ssl/x509/mkcert.py --config certs.yml`
Optionally providing a cert ID to only regenerate a single cert.
"""
import argparse
import binascii
import datetime
import os
import random
import subprocess
import tempfile
from typing import Any, Dict
import yaml
import OpenSSL

# pylint: disable=protected-access
OpenSSL._util.lib.OBJ_create(b'1.2.3.45', b'DummyOID45', b'Dummy OID 45')
OpenSSL._util.lib.OBJ_create(b'1.2.3.56', b'DummyOID56', b'Dummy OID 56')
OpenSSL._util.lib.OBJ_create(b'1.3.6.1.4.1.34601.2.1.1', b'mongoRoles',
                              b'Sequence of MongoDB Database Roles')
# pylint: enable=protected-access

CONFIGFILE = 'jstests/ssl/x509/certs.yml'

CONFIG = Dict[str, Any]

def glbl(key, default=None):
    """Fetch a key from the global dict."""
    return CONFIG.get('global', {}).get(key, default)

def idx(cert, key, default=None):
    """Fetch a key from the cert dict, falling back through global dict."""
    return cert.get(key, None) or glbl(key, default)

def make_key(cert):
    """Generate an RSA or DSA private key."""
    # Note that ECDSA keys are generated in the
    # process_csdsa_*() functions below.
    type_str = idx(cert, 'key_type', 'RSA')
    if type_str == 'RSA':
        key_type = OpenSSL.crypto.TYPE_RSA
    elif type_str == 'DSA':
        key_type = OpenSSL.crypto.TYPE_DSA
    else:
        raise ValueError('Unknown key_type: ' + type_str)

    key_size = int(idx(cert, 'key_size', '2048'))
    if key_size < 1024:
        raise ValueError('Invalid key_size: ' + key_size)

    key = OpenSSL.crypto.PKey()
    key.generate_key(key_type, key_size)
    return key

def make_filename(cert):
    """Form a pathname from a certificate name."""
    return idx(cert, 'output_path') + '/' + cert['name']

def find_certificate_definition(name):
    """Locate a definition by name."""
    for ca_cert in CONFIG['certs']:
        if ca_cert['name'] == name:
            return ca_cert

    return None

def get_cert_path(name):
    """Determine certificate path by name."""
    entry = find_certificate_definition(name)
    return make_filename(entry) if entry else name

def load_authority_file(issuer):
    """Locate the cert/key file for a given ID and load their parts."""
    ca_cert = find_certificate_definition(issuer)
    if ca_cert:
        pem = open(make_filename(ca_cert), 'rt').read()
        certificate = OpenSSL.crypto.load_certificate(OpenSSL.crypto.FILETYPE_PEM, pem)
        passphrase = ca_cert.get('passphrase', None)
        if passphrase:
            passphrase = passphrase.encode('utf-8')

        signing_key = OpenSSL.crypto.load_privatekey(OpenSSL.crypto.FILETYPE_PEM, pem, passphrase=passphrase)
        return (certificate, signing_key)

    # Externally sourced certifiate, try by path. Hopefully unencrypted.
    # pylint: disable=bare-except
    try:
        pem = open(issuer, 'rt').read()
        certificate = OpenSSL.crypto.load_certificate(OpenSSL.crypto.FILETYPE_PEM, pem)
        signing_key = OpenSSL.crypto.load_privatekey(OpenSSL.crypto.FILETYPE_PEM, pem)
        return (certificate, signing_key)
    except:
        pass

    return (None, None)

def set_subject(x509, cert):
    """Translate a subject dict to X509Name elements."""
    if cert.get('Subject', None) is None:
        raise ValueError(cert['name'] + ' requires a Subject')

    if not cert.get('explicit_subject', False):
        for key, val in glbl('Subject', {}).items():
            setattr(x509.get_subject(), key, val)

    for key, val in cert['Subject'].items():
        setattr(x509.get_subject(), key, val)

def set_validity(x509, cert):
    """Set validity range for the certificate."""
    not_before = idx(cert, 'not_before', None)
    if not_before:
        # TODO: Parse human readable dates and/or datedeltas
        not_before = int(not_before)
    else:
        # Default last week.
        not_before = -7 * 24 * 60 * 60
    x509.gmtime_adj_notBefore(not_before)

    not_after = idx(cert, 'not_after', None)
    if not_after:
        # TODO: Parse human readable dates and/or datedeltas
        not_after = int(not_after)
    else:
        # Default 20 years hence.
        not_after = 20 * 365 * 24 * 60 * 60
    x509.gmtime_adj_notAfter(not_after)

def set_general_dict_extension(x509, exts, cert, name, typed_values):
    """Set dict key/value pairs for an extension."""
    tags = cert.get('extensions', {}).get(name, {})
    if not tags:
        return

    critical = False
    value = []
    for key, val in tags.items():
        if key == 'critical':
            if not val is True:
                raise ValueError('critical must be precisely equal to TRUE')

            critical = True
            continue

        if not key in typed_values:
            raise ValueError('Unknown key for extensions. ' + name + ': ' + key)

        if not isinstance(val, type(typed_values[key])):
            raise ValueError('Type mismatch for extensions. ' + name + '.' + key)

        if isinstance(val, bool):
            value.append(key + ':' + ('TRUE' if val else 'FALSE'))
        else:
            value.append(key + ':' + val)

    exts.append(OpenSSL.crypto.X509Extension(b'basicConstraints', critical, ','.join(value).encode('utf-8'), subject=x509))

def set_general_list_extension(x509, exts, cert, name, values):
    """Set value elements for a given extension."""
    tags = cert.get('extensions', {}).get(name, ())
    if not tags:
        return

    if isinstance(tags, str):
        tags = [tags]

    critical = False
    if 'critical' in tags:
        critical = True
        tags.remove('critical')

    for key in tags:
        if key not in values:
            raise ValueError('Illegal tag: ' + key)

    exts.append(OpenSSL.crypto.X509Extension(name.encode('utf-8'), critical, ','.join(tags).encode('utf-8'), subject=x509))

def set_san_extension(x509, exts, cert):
    """Set the Subject Alternate Name extension."""
    san = cert.get('extensions', {}).get('subjectAltName')
    if not san:
        return

    critical = False
    sans = []
    for typ, vals in san.items():
        if typ == 'critical':
            if not vals is True:
                raise ValueError('critical must be precisely equal to TRUE')
            critical = True
            continue

        if not typ in ['IP', 'DNS']: # Other things can live here, but this is all we use.
            raise ValueError('Fix me? Only IP and DNS SANs are handled')

        if not isinstance(vals, list):
            vals = [vals]

        for val in vals:
            sans.append(typ + ':' + val)

    if not sans:
        return

    exts.append(OpenSSL.crypto.X509Extension(b'subjectAltName', critical, ','.join(sans).encode('utf-8'), subject=x509))

def enable_subject_key_identifier_extension(x509, exts, cert):
    """Enable the subject key identifier extension."""
    ident = cert.get('extensions', {}).get('subjectKeyIdentifier', False)
    if not ident:
        return
    if ident not in ['hash', 'hash-critical']:
        raise ValueError("Only the value 'hash' is accepted for subejctKeyIdentifier")

    exts.append(OpenSSL.crypto.X509Extension(b'subjectKeyIdentifier', ident == 'hash-critical', b'hash', subject=x509))

def enable_authority_key_identifier_extension(x509, exts, cert):
    """Enable the authority key identifier extension."""
    ident = cert.get('extensions', {}).get('authorityKeyIdentifier', False)
    if not ident:
        return

    if ident not in ['keyid', 'issuer']:
        raise ValueError("Only the 'keyid' or 'issuer' values are accepted for authorityKeyIdentifier")
    issuer = cert.get('Issuer', 'ca.pem')
    if issuer == 'self':
        issuer_cert = x509
    else:
        issuer_cert = load_authority_file(issuer)[0]

    exts.append(OpenSSL.crypto.X509Extension(b'authorityKeyIdentifier', False, ident.encode('utf-8'), subject=x509, issuer=issuer_cert))

def to_der_varint(val):
    """Translate a native int to a variable length ASN.1 encoded integer."""
    if val < 0:
        raise ValueError('Negative values nor permitted in DER payload')

    if val < 0x80:
        return chr(val).encode('ascii')

    ret = bytearray(b'')
    while (val > 0) and (len(ret) < 8):
        ret.insert(0, val & 0xFF)
        val = val >> 8

    if val > 0:
        raise ValueError('Length is too large to represent in 64bits')

    ret.insert(0, 0x80 + len(ret))
    return ret

def to_der_utf8_string(val):
    """Encode a unicode string as a ASN.1 UTF8 String."""
    utf8_val = str(val).encode('utf-8')
    return b'\x0C' + to_der_varint(len(utf8_val)) + utf8_val

def to_der_sequence_pair(name, value):
    """Encode a pair of ASN.1 values as a sequence pair."""
    # Simplified sequence which always expects two string, a key and a value.
    bin_name = to_der_utf8_string(name)
    bin_value = to_der_utf8_string(value)
    return b'\x30' + to_der_varint(len(bin_name) + len(bin_value)) + bin_name + bin_value

def set_mongo_roles_extension(exts, cert):
    """Encode a set of role/db pairs into a MongoDB DER packet."""
    roles = cert.get('extensions', {}).get('mongoRoles')
    if not roles:
        return

    pair = b''
    for role in roles:
        if (len(role) != 2) or ('role' not in role) or ('db' not in role):
            raise ValueError('mongoRoles must consist of a series of role/db pairs')
        pair = pair + to_der_sequence_pair(role['role'], role['db'])

    value = b'DER:31' + binascii.hexlify(to_der_varint(len(pair))) + binascii.hexlify(pair)

    exts.append(OpenSSL.crypto.X509Extension(b'1.3.6.1.4.1.34601.2.1.1', False, value))

def set_extensions(x509, cert):
    """Setup X509 extensions."""
    exts = []
    set_general_dict_extension(x509, exts, cert, 'basicConstraints', {'CA': False, 'pathlen': 0})
    set_general_list_extension(x509, exts, cert, 'keyUsage', [
        'digitalSignature', 'nonRepudiation', 'keyEncipherment', 'dataEncipherment',
        'keyAgreement', 'keyCertSign', 'cRLSign', 'encipherOnly', 'decipherOnly'])
    set_general_list_extension(x509, exts, cert, 'extendedKeyUsage', [
        'serverAuth', 'clientAuth', 'codeSigning', 'emailProtection', 'timeStamping',
        'msCodeInd', 'msCodeCom', 'msCTLSign', 'msSGC', 'msEFS', 'nsSGC'])
    enable_subject_key_identifier_extension(x509, exts, cert)
    enable_authority_key_identifier_extension(x509, exts, cert)
    set_san_extension(x509, exts, cert)
    set_mongo_roles_extension(exts, cert)

    ns_comment = cert.get('extensions', {}).get('nsComment')
    if ns_comment:
        exts.append(OpenSSL.crypto.X509Extension(b'nsComment', False, ns_comment.encode('utf-8')))

    if exts:
        x509.add_extensions(exts)

def sign_cert(x509, cert, key):
    """Sign the new certificate."""
    sig = idx(cert, 'hash', 'sha256')

    issuer = cert.get('Issuer', 'ca.pem')
    if issuer == 'self':
        x509.set_issuer(x509.get_subject())
        x509.sign(key, sig)
        return

    # Signed by a CA, find the key...
    (signing_cert, signing_key) = load_authority_file(issuer)

    if not signing_key:
        raise ValueError('No issuer available to sign with')

    x509.set_issuer(signing_cert.get_subject())
    x509.sign(signing_key, sig)

def get_header_comment(cert):
    """Header comment for every generated file."""
    comment = "# Autogenerated file, do not edit.\n"
    comment = comment + '# Generate using jstests/ssl/x509/mkcert.py --config ' + CONFIGFILE
    comment = comment + ' '  + cert['name'] + "\n#\n"
    comment = comment + "# " + cert.get('description', '').replace("\n", "\n# ")
    return comment

def convert_cert_to_pkcs1(cert):
    """Reencodes the main certificate to use PKCS#1 private key encryption."""
    src = make_filename(cert)
    pswd = 'pass:' + cert['passphrase']

    tmpcert = tempfile.mkstemp()[1]
    tmpkey = tempfile.mkstemp()[1]
    subprocess.check_call(['openssl', 'x509', '-in', src, '-out', tmpcert])
    subprocess.check_call(['openssl', 'rsa', '-in', src, '-passin', pswd, '-out', tmpkey, '-aes256', '-passout', pswd])
    open(src, 'wt').write(get_header_comment(cert) + "\n" + open(tmpcert, 'rt').read() + open(tmpkey, 'rt').read())
    os.remove(tmpcert)
    os.remove(tmpkey)

def convert_cert_to_pkcs12(cert):
    """Makes a new copy of the cert/key pair using PKCS#12 encoding."""
    pkcs12 = cert.get('pkcs12')
    if not pkcs12.get('passphrase'):
        raise ValueError('PKCS#12 requires a passphrase')

    src = make_filename(cert)
    dest = idx(cert, 'output_path') + '/' + pkcs12.get('name', cert['name'])
    ca = get_cert_path(cert['Issuer'])
    passout = 'pass:' + pkcs12['passphrase']

    args = ['openssl', 'pkcs12', '-export', '-in', src, '-inkey', src, '-out', dest, '-passout', passout, '-certfile', ca]
    if cert.get('passphrase'):
        args.append('-passin')
        args.append('pass:' + cert['passphrase'])

    subprocess.check_call(args)

def create_cert(cert):
    """Create a new X509 certificate."""
    x509 = OpenSSL.crypto.X509()
    key = make_key(cert)
    x509.set_pubkey(key)

    set_subject(x509, cert)
    set_validity(x509, cert)
    set_extensions(x509, cert)
    # Serial numbers 0..999 are reserved for fixed serial numbers.
    # Other will be assigned randomly.
    x509.set_serial_number(cert.get('serial', random.randint(1000, 0x7FFFFFFF)))

    sign_cert(x509, cert, key)

    passphrase = cert.get('passphrase', None)
    cipher = None
    if passphrase:
        passphrase = passphrase.encode('utf-8')
        cipher = 'aes256'

    open(make_filename(cert), 'wt').write(
        get_header_comment(cert) + "\n" +
        OpenSSL.crypto.dump_certificate(OpenSSL.crypto.FILETYPE_PEM, x509).decode('ascii') +
        OpenSSL.crypto.dump_privatekey(OpenSSL.crypto.FILETYPE_PEM, key, cipher=cipher, passphrase=passphrase).decode('ascii'))

    if cert.get('pkcs1'):
        convert_cert_to_pkcs1(cert)

    if cert.get('pkcs12'):
        convert_cert_to_pkcs12(cert)

def check_special_case_keys(cert):
    """All special cases must contain precisely these keys."""
    for key in cert.keys():
        if not key in ['name', 'description', 'Issuer']:
            raise ValueError('Unexpected field in special entry: ' + key)

    if len(cert) != 3:
        raise ValueError('Missing field in special entry')

def process_client_multivalue_rdn(cert):
    """Special handling for client-multivalue-rdn.pem"""
    check_special_case_keys(cert)

    key = tempfile.mkstemp()[1]
    csr = tempfile.mkstemp()[1]
    rsa = tempfile.mkstemp()[1]
    pem = tempfile.mkstemp()[1]
    dest = make_filename(cert)

    ca = get_cert_path(cert['Issuer'])
    serial = str(random.randint(1000, 0x7FFFFFFF))
    subject = '/CN=client+OU=KernelUser+O=MongoDB/L=New York City+ST=New York+C=US'
    subprocess.check_call(['openssl', 'req', '-new', '-nodes', '-multivalue-rdn', '-subj', subject, '-keyout', key, '-out', csr])
    subprocess.check_call(['openssl', 'rsa', '-in', key, '-out', rsa])
    subprocess.check_call(['openssl', 'x509', '-in', csr, '-out', pem, '-req', '-CA', ca, '-CAkey', ca, '-days', '3650', '-sha256', '-set_serial', serial])

    open(dest, 'wt').write(get_header_comment(cert) + "\n" + open(pem, 'rt').read() + open(rsa, 'rt').read())
    os.remove(key)
    os.remove(csr)
    os.remove(rsa)
    os.remove(pem)

def process_ecdsa_ca(cert):
    """Create CA for ECDSA tree."""
    check_special_case_keys(cert)
    if cert['Issuer'] != 'self':
        raise ValueError('ECDSA-CA should be self-signed')

    key = tempfile.mkstemp()[1]
    csr = tempfile.mkstemp()[1]
    pem = tempfile.mkstemp()[1]
    dest = make_filename(cert)

    serial = str(random.randint(1000, 0x7FFFFFFF))
    subject = '/C=US/ST=New York/L=New York City/O=MongoDB/OU=Kernel/CN=Kernel Test ESCDA CA/'
    subprocess.check_call(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-out', key])
    subprocess.check_call(['openssl', 'req', '-new', '-key', key, '-out', csr, '-subj', subject])
    subprocess.check_call(['openssl', 'x509', '-in', csr, '-out', pem, '-req', '-signkey', key, '-days', '7300', '-sha256', '-set_serial', serial])

    open(dest, 'wt').write(get_header_comment(cert) + "\n" + open(pem, 'rt').read() + open(key, 'rt').read())
    os.remove(key)
    os.remove(csr)
    os.remove(pem)

def process_ecdsa_leaf(cert):
    """Create leaf certificates for ECDSA tree."""
    check_special_case_keys(cert)

    key = tempfile.mkstemp()[1]
    csr = tempfile.mkstemp()[1]
    pem = tempfile.mkstemp()[1]
    extfile = None
    dest = make_filename(cert)

    ca = get_cert_path(cert['Issuer'])
    serial = str(random.randint(1000, 0x7FFFFFFF))
    mode = 'client' if cert['name'] == 'ecdsa-client.pem' else 'server'
    ou = 'Kernel' if mode == 'server' else 'KernelUser'
    subject = '/C=US/ST=New York/L=New York City/O=MongoDB/OU=' + ou + '/CN=' + mode

    reqargs = ['openssl', 'req', '-new', '-key', key, '-out', csr, '-subj', subject]
    x509args = ['openssl', 'x509', '-in', csr, '-out', pem, '-req', '-CA', ca, '-CAkey', ca, '-days', '7300', '-sha256', '-set_serial', serial]
    if mode == 'server':
        reqargs.append('-reqexts=v3_req')
        extfile = tempfile.mkstemp()[1]
        open(extfile, 'wt').write('subjectAltName=DNS:localhost,DNS:127.0.0.1')
        x509args.append('-extfile')
        x509args.append(extfile)

    subprocess.check_call(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-out', key])
    subprocess.check_call(reqargs)
    subprocess.check_call(x509args)

    open(dest, 'wt').write(get_header_comment(cert) + "\n" + open(pem, 'rt').read() + open(key, 'rt').read())
    os.remove(key)
    os.remove(csr)
    os.remove(pem)
    if extfile:
        os.remove(extfile)

def process_cert(cert):
    """Process a certificate."""
    print('Processing certificate: ' + cert['name'])

    if cert['name'] == 'client-multivalue-rdn.pem':
        process_client_multivalue_rdn(cert)
        return

    if cert['name'] == 'ecdsa-ca.pem':
        process_ecdsa_ca(cert)
        return

    if cert['name'] in ['ecdsa-client.pem', 'ecdsa-server.pem']:
        process_ecdsa_leaf(cert)
        return

    append_certs = cert.get('append_cert', [])
    if isinstance(append_certs, str):
        append_certs = [append_certs]

    if cert.get('Subject'):
        create_cert(cert)
    elif append_certs:
        # Pure composing certificate, unlink first so that the append loop doesn't retain old versions.
        os.remove(make_filename(cert))
    else:
        raise ValueError("Certificate definitions must have at least one of 'Subject' and/or 'append_cert'")

    for append_cert in append_certs:
        x509 = load_authority_file(append_cert)[0]
        if not x509:
            raise ValueError("Unable to find certificate '" + append_cert + "' to append")
        open(make_filename(cert), 'at').write(
            "# Certificate from " + append_cert + "\n" +
            OpenSSL.crypto.dump_certificate(OpenSSL.crypto.FILETYPE_PEM, x509).decode('ascii'))

def parse_command_line():
    """Accept a named config file."""
    # pylint: disable=global-statement
    global CONFIGFILE

    parser = argparse.ArgumentParser(description='X509 Test Certificate Generator')
    parser.add_argument('--config', help='Certificate definition file', type=str, default=CONFIGFILE)
    parser.add_argument('cert', nargs='*', help='Certificate to generate (blank for all)')
    args = parser.parse_args()
    CONFIGFILE = args.config
    return args.cert or []

def validate_config():
    """Perform basic start up time validation of config file."""
    if not glbl('output_path'):
        raise ValueError('global.output_path required')

    if not CONFIG.get('certs'):
        raise ValueError('No certificates defined')

    permissible = ['name', 'description', 'Subject', 'Issuer', 'append_cert', 'extensions', 'passphrase', 'output_path', 'hash', 'key_type', 'explicit_subject', 'serial', 'not_before', 'not_after', 'pkcs1', 'pkcs12']
    for cert in CONFIG.get('certs', []):
        keys = cert.keys()
        if not 'name' in keys:
            raise ValueError('Name field required for all certificate definitions')
        if not 'description' in keys:
            raise ValueError('description field required for all certificate definitions')
        for key in keys:
            if not key in permissible:
                raise ValueError("Unknown element '" + key + "' in certificate: " + cert['name'])

def select_items(names):
    """Select all certificates requested and their leaf nodes."""
    if not names:
        return CONFIG['certs']

    # Temporarily treat like dictionary for easy de-duping.
    ret = {}
    # Start with the cert(s) explicitly asked for.
    for name in names:
        cert = find_certificate_definition(name)
        if not cert:
            raise ValueError('Unknown certificate: ' + name)
        ret[name] = cert

    last_count = -1
    while last_count != len(ret):
        last_count = len(ret)
        # Add any certs who use our current set as an issuer.
        ret.update({cert['name']: cert for cert in CONFIG['certs'] if cert.get('Issuer') in names})
        # Add any certs who are composed of our current set.
        ret.update({cert['name']: cert for cert in CONFIG['certs'] if [True for append in cert.get('append_cert', []) if append in names]})
        # Repeat until no new names are added.
        names = ret.keys()

    return ret.values()

def sort_items(items):
    """Ensure that leaves are produced after roots (as much as possible within one file)."""
    all_names = [cert['name'] for cert in items]
    all_names.sort()
    processed_names = []

    ret = []
    while len(ret) != len(items):
        for cert in items:
            # only concern ourselves with prependents in this config file.
            unmet_prependents = [name for name in cert.get('append_certs', []) if (name in all_names) and (not name in processed_names)]

            # Self-signed, signed by someone in ret already, or signed externally
            issuer = cert.get('Issuer')
            has_issuer = (issuer == 'self') or (issuer in processed_names) or (issuer not in all_names)

            if has_issuer and not unmet_prependents:
                ret.append(cert)
                processed_names.append(cert['name'])

    return ret

def main():
    """Go go go."""
    # pylint: disable=global-statement
    global CONFIG

    items_to_process = parse_command_line()
    CONFIG = yaml.load(open(CONFIGFILE, 'r'))
    validate_config()
    items = select_items(items_to_process)
    items = sort_items(items)
    for item in items:
        process_cert(item)

if __name__ == '__main__':
    main()
