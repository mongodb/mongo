#!/usr/bin/env python

import logging
import os
import os.path
import re
import shutil
import sys
import tempfile
import time
import unittest
import random
from platform import architecture

from cli_common import (file_text, find_utility, is_windows, list_upto,
                        path_for_gpg, pswd_pipe, raise_err, random_text,
                        run_proc, decode_string_escape, CONSOLE_ENCODING,
                        set_workdir)
from gnupg import GnuPG as GnuPG
from rnp import Rnp as Rnp

WORKDIR = ''
RNP = ''
RNPK = ''
GPG = ''
GPGCONF = ''
RNPDIR = ''
RNPDIR2 = ''
GPGHOME = None
PASSWORD = 'password'
RMWORKDIR = True
GPG_AEAD = False
GPG_AEAD_EAX = False
GPG_AEAD_OCB = False
GPG_NO_OLD = False
GPG_BRAINPOOL = False
TESTS_SUCCEEDED = []
TESTS_FAILED = []
TEST_WORKFILES = []

# Supported features
RNP_TWOFISH = True
RNP_BRAINPOOL = True
RNP_AEAD_EAX = True
RNP_AEAD_OCB = True
RNP_AEAD_OCB_AES = False
RNP_AEAD = True
RNP_IDEA = True
RNP_BLOWFISH = True
RNP_CAST5 = True
RNP_RIPEMD160 = True
RNP_SM2 = True
# Botan may cause AV during OCB decryption in certain cases, see https://github.com/randombit/botan/issues/3812
RNP_BOTAN_OCB_AV = False
RNP_BACKEND = ''

if sys.version_info >= (3,):
    unichr = chr

def escape_regex(str):
    return '^' + ''.join((c, "[\\x{:02X}]".format(ord(c)))[0 <= ord(c) <= 0x20 \
        or c in ['[',']','(',')','|','"','$','.','*','^','$','\\','+','?','{','}']] for c in str) + '$'

UNICODE_LATIN_CAPITAL_A_GRAVE = unichr(192)
UNICODE_LATIN_SMALL_A_GRAVE = unichr(224)
UNICODE_LATIN_CAPITAL_A_MACRON = unichr(256)
UNICODE_LATIN_SMALL_A_MACRON = unichr(257)
UNICODE_GREEK_CAPITAL_HETA = unichr(880)
UNICODE_GREEK_SMALL_HETA = unichr(881)
UNICODE_GREEK_CAPITAL_OMEGA = unichr(937)
UNICODE_GREEK_SMALL_OMEGA = unichr(969)
UNICODE_CYRILLIC_CAPITAL_A = unichr(0x0410)
UNICODE_CYRILLIC_SMALL_A = unichr(0x0430)
UNICODE_CYRILLIC_CAPITAL_YA = unichr(0x042F)
UNICODE_CYRILLIC_SMALL_YA = unichr(0x044F)
UNICODE_SEQUENCE_1 = UNICODE_LATIN_CAPITAL_A_GRAVE + UNICODE_LATIN_SMALL_A_MACRON \
    + UNICODE_GREEK_CAPITAL_HETA + UNICODE_GREEK_SMALL_OMEGA \
    + UNICODE_CYRILLIC_CAPITAL_A + UNICODE_CYRILLIC_SMALL_YA
UNICODE_SEQUENCE_2 = UNICODE_LATIN_SMALL_A_GRAVE + UNICODE_LATIN_CAPITAL_A_MACRON \
    + UNICODE_GREEK_SMALL_HETA + UNICODE_GREEK_CAPITAL_OMEGA \
    + UNICODE_CYRILLIC_SMALL_A + UNICODE_CYRILLIC_CAPITAL_YA
WEIRD_USERID_UNICODE_1 = unichr(160) + unichr(161) \
    + UNICODE_SEQUENCE_1 + unichr(40960) + u'@rnp'
WEIRD_USERID_UNICODE_2 = unichr(160) + unichr(161) \
    + UNICODE_SEQUENCE_2 + unichr(40960) + u'@rnp'
WEIRD_USERID_SPECIAL_CHARS = '\\}{][)^*.+(\t\n|$@rnp'
WEIRD_USERID_SPACE = ' '
WEIRD_USERID_QUOTE = '"'
WEIRD_USERID_SPACE_AND_QUOTE = ' "'
WEIRD_USERID_QUOTE_AND_SPACE = '" '
WEIRD_USERID_TOO_LONG = 'x' * 125 + '@rnp' # totaling 129 (MAX_USER_ID + 1)

# Key userids
KEY_ENCRYPT = 'encryption@rnp'
KEY_SIGN_RNP = 'signing@rnp'
KEY_SIGN_GPG = 'signing@gpg'
KEY_ENC_RNP = 'enc@rnp'
AT_EXAMPLE = '@example.com'

# Keyrings
PUBRING = 'pubring.gpg'
SECRING = 'secring.gpg'
PUBRING_1 = 'keyrings/1/pubring.gpg'
SECRING_1 = 'keyrings/1/secring.gpg'
KEYRING_DIR_1 = 'keyrings/1'
KEYRING_DIR_2 = 'keyrings/2'
KEYRING_DIR_3 = 'keyrings/3'
PUBRING_7 = 'keyrings/7/pubring.gpg'
SECRING_G10 = 'test_stream_key_load/g10'
KEY_ALICE_PUB = 'test_key_validity/alice-pub.asc'
KEY_ALICE_SUB_PUB = 'test_key_validity/alice-sub-pub.pgp'
KEY_ALICE_SEC = 'test_key_validity/alice-sec.asc'
KEY_ALICE_SUB_SEC = 'test_key_validity/alice-sub-sec.pgp'
KEY_ALICE = 'Alice <alice@rnp>'
KEY_25519_NOTWEAK_SEC = 'test_key_edge_cases/key-25519-non-tweaked-sec.asc'

# Messages
MSG_TXT = 'test_messages/message.txt'
MSG_ES_25519 = 'test_messages/message.txt.enc-sign-25519'
MSG_SIG_CRCR = 'test_messages/message.text-sig-crcr.sig'

# Extensions
EXT_SIG = '.txt.sig'
EXT_ASC = '.txt.asc'
EXT_PGP = '.txt.pgp'

# Misc
GPG_LOOPBACK = '--pinentry-mode=loopback'

# Regexps
RE_RSA_KEY = r'(?s)^' \
r'# .*' \
r':public key packet:\s+' \
r'version 4, algo 1, created \d+, expires 0\s+' \
r'pkey\[0\]: \[(\d{4}) bits\]\s+' \
r'pkey\[1\]: \[17 bits\]\s+' \
r'keyid: ([0-9A-F]{16})\s+' \
r'# .*' \
r':user ID packet: "(.+)"\s+' \
r'# .*' \
r':signature packet: algo 1, keyid \2\s+' \
r'.*' \
r'# .*' \
r':public sub key packet:' \
r'.*' \
r':signature packet: algo 1, keyid \2\s+' \
r'.*$'

RE_RSA_KEY_LIST = r'^\s*' \
r'2 keys found\s+' \
r'pub\s+(\d{4})/RSA ([0-9a-z]{16}) \d{4}-\d{2}-\d{2} \[.*\]\s+' \
r'([0-9a-z]{40})\s+' \
r'uid\s+(.+)\s+' \
r'sub.+\s+' \
r'[0-9a-z]{40}\s+$'

RE_MULTIPLE_KEY_LIST = r'(?s)^\s*(\d+) (?:key|keys) found.*$'
RE_MULTIPLE_KEY_5 = r'(?s)^\s*' \
r'10 keys found.*' \
r'.+uid\s+0@rnp-multiple' \
r'.+uid\s+1@rnp-multiple' \
r'.+uid\s+2@rnp-multiple' \
r'.+uid\s+3@rnp-multiple' \
r'.+uid\s+4@rnp-multiple.*$'

RE_MULTIPLE_SUBKEY_3 = r'(?s)^\s*' \
r'3 keys found.*$'

RE_MULTIPLE_SUBKEY_8 = r'(?s)^\s*' \
r'8 keys found.*$'

RE_GPG_SINGLE_RSA_KEY = r'(?s)^\s*' \
r'.+-+\s*' \
r'pub\s+rsa.+' \
r'\s+([0-9A-F]{40})\s*' \
r'uid\s+.+rsakey@gpg.*'

RE_GPG_GOOD_SIGNATURE = r'(?s)^.*' \
r'gpg: Signature made .*' \
r'gpg: Good signature from "(.*)".*'

RE_RNP_GOOD_SIGNATURE = r'(?s)^.*' \
r'Good signature made .*' \
r'using .* key .*' \
r'pub .*' \
r'uid\s+(.*)\s*' \
r'Signature\(s\) verified successfully.*$'

RE_RNP_ENCRYPTED_KEY = r'(?s)^.*' \
r'Secret key packet.*' \
r'secret key material:.*' \
r'encrypted secret key data:.*' \
r'UserID packet.*' \
r'id: enc@rnp.*' \
r'Secret subkey packet.*' \
r'secret key material:.*' \
r'encrypted secret key data:.*$'

RE_RNP_REVOCATION_SIG = r'(?s)' \
r':armored input\n' \
r':off 0: packet header .* \(tag 2, len .*' \
r'Signature packet.*' \
r'version: 4.*' \
r'type: 32 \(Key revocation signature\).*' \
r'public key algorithm:.*' \
r'hashed subpackets:.*' \
r':type 33, len 21.*' \
r'issuer fingerprint:.*' \
r':type 2, len 4.*' \
r'signature creation time:.*' \
r':type 29.*' \
r'reason for revocation: (.*)' \
r'message: (.*)' \
r'unhashed subpackets:.*' \
r':type 16, len 8.*' \
r'issuer key ID: .*$'

RE_GPG_REVOCATION_IMPORT = r'(?s)^.*' \
r'key 0451409669FFDE3C: "Alice <alice@rnp>" revocation certificate imported.*' \
r'Total number processed: 1.*' \
r'new key revocations: 1.*$'

RE_SIG_1_IMPORT = r'(?s)^.*Import finished: 1 new signature, 0 unchanged, 0 unknown.*'

RE_KEYSTORE_INFO = r'(?s)^.*fatal: cannot set keystore info'

RNP_TO_GPG_ZALGS = { 'zip' : '1', 'zlib' : '2', 'bzip2' : '3' }
# These are mostly identical
RNP_TO_GPG_CIPHERS = {'AES' : 'aes128', 'AES192' : 'aes192', 'AES256' : 'aes256',
                      'TWOFISH' : 'twofish', 'CAMELLIA128' : 'camellia128',
                      'CAMELLIA192' : 'camellia192', 'CAMELLIA256' : 'camellia256',
                      'IDEA' : 'idea', '3DES' : '3des', 'CAST5' : 'cast5',
                      'BLOWFISH' : 'blowfish'}

# Error messages
RNP_DATA_DIFFERS = 'rnp decrypted data differs'
GPG_DATA_DIFFERS = 'gpg decrypted data differs'
KEY_GEN_FAILED = 'key generation failed'
KEY_LIST_FAILED = 'key list failed'
KEY_LIST_WRONG = 'wrong key list output'
PKT_LIST_FAILED = 'packet listing failed'
ALICE_IMPORT_FAIL = 'Alice key import failed'
ENC_FAILED = 'encryption failed'
DEC_FAILED = 'decryption failed'
DEC_DIFFERS = 'Decrypted data differs'
GPG_IMPORT_FAILED = 'gpg key import failed'

def check_packets(fname, regexp):
    ret, output, err = run_proc(GPG, ['--homedir', '.',
                                      '--list-packets', path_for_gpg(fname)])
    if ret != 0:
        logging.error(err)
        return None
    else:
        result = re.match(regexp, output)
        if not result:
            logging.debug('Wrong packets:')
            logging.debug(output)
        return result

def clear_keyrings():
    shutil.rmtree(RNPDIR, ignore_errors=True)
    os.mkdir(RNPDIR, 0o700)

    run_proc(GPGCONF, ['--homedir', GPGHOME, '--kill', 'gpg-agent'])
    while os.path.isdir(GPGDIR):
        try:
            shutil.rmtree(GPGDIR)
        except Exception:
            time.sleep(0.1)
    os.mkdir(GPGDIR, 0o700)

def allow_y2k38_on_32bit(filename):
    if architecture()[0] == '32bit':
        return [filename, filename + '_y2k38']
    else:
        return [filename]

def compare_files(src, dst, message):
    if file_text(src) != file_text(dst):
        raise_err(message)

def compare_file(src, string, message):
    if file_text(src) != string:
        raise_err(message)

def compare_file_any(srcs, string, message):
    for src in srcs:
        if file_text(src) == string:
            return
    raise_err(message)

def compare_file_ex(src, string, message, symbol='?'):
    ftext = file_text(src)
    if len(ftext) != len(string):
        raise_err(message)
    for i in range(0, len(ftext)):
        if (ftext[i] != symbol[0]) and (ftext[i] != string[i]):
            raise_err(message)

def remove_files(*args):
    for fpath in args:
        try:
            os.remove(fpath)
        except Exception:
            # Ignore if file cannot be removed
            pass

def reg_workfiles(mainname, *exts):
    global TEST_WORKFILES
    res = []
    for ext in exts:
        fpath = os.path.join(WORKDIR, mainname + ext)
        if fpath in TEST_WORKFILES:
            logging.warn('Warning! Path {} is already in TEST_WORKFILES'.format(fpath))
        else:
            TEST_WORKFILES += [fpath]
        res += [fpath]
    return res

def clear_workfiles():
    global TEST_WORKFILES
    remove_files(*TEST_WORKFILES)
    TEST_WORKFILES = []

def rnp_genkey_rsa(userid, bits=2048, pswd=PASSWORD):
    pipe = pswd_pipe(pswd)
    ret, _, err = run_proc(RNPK, ['--numbits', str(bits), '--homedir', RNPDIR, '--pass-fd', str(pipe),
                                  '--notty', '--s2k-iterations', '50000', '--userid', userid, '--generate-key'])
    os.close(pipe)
    if ret != 0:
        raise_err('rsa key generation failed', err)

def rnp_genkey_pqc(userid, algo_cli_nr, homedir, algo_param = None, pswd=PASSWORD):
    algo_pipe = str(algo_cli_nr)
    if algo_param:
        algo_pipe += "\n" + str(algo_param)
    ret, out, err = run_proc(RNPK, ['--homedir', homedir, '--password', pswd,
                                  '--notty', '--userid', userid, '--generate-key', '--expert'], algo_pipe)
    #os.close(algo_pipe)
    if ret != 0:
        raise_err('pqc key generation failed', err)
    return out

def rnp_params_insert_z(params, pos, z):
    if z:
        if len(z) > 0 and z[0] != None:
            params[pos:pos] = ['--' + z[0]]
        if len(z) > 1 and z[1] != None:
            params[pos:pos] = ['-z', str(z[1])]

def rnp_params_insert_aead(params, pos, aead):
    if aead != None:
        params[pos:pos] = ['--aead=' + aead[0]] if len(aead) > 0 and aead[0] != None else ['--aead']
        if len(aead) > 1 and aead[1] != None:
            params[pos + 1:pos + 1] = ['--aead-chunk-bits=' + str(aead[1])]

def rnp_encrypt_file_ex(src, dst, recipients=None, passwords=None, aead=None, cipher=None,
                        z=None, armor=False, s2k_iter=False, s2k_msec=False):
    params = ['--homedir', RNPDIR, src, '--output', dst, '--allow-old-ciphers']
    # Recipients. None disables PK encryption, [] to use default key. Otherwise list of ids.
    if recipients != None:
        params[2:2] = ['--encrypt']
        for userid in reversed(recipients):
            params[2:2] = ['-r', escape_regex(userid)]
    # Passwords to encrypt to. None or [] disables password encryption.
    if passwords:
        if recipients is None:
            params[2:2] = ['-c']
        if s2k_iter != False:
            params += ['--s2k-iterations', str(s2k_iter)]
        if s2k_msec != False:
            params += ['--s2k-msec', str(s2k_msec)]
        pipe = pswd_pipe('\n'.join(passwords))
        params[2:2] = ['--pass-fd', str(pipe), '--passwords', str(len(passwords))]

    # Cipher or None for default
    if cipher: params[2:2] = ['--cipher', cipher]
    # Armor
    if armor: params += ['--armor']
    rnp_params_insert_aead(params, 2, aead)
    rnp_params_insert_z(params, 2, z)
    ret, _, err = run_proc(RNP, params)
    if passwords: os.close(pipe)
    if ret != 0:
        raise_err('rnp encryption failed with ' + cipher, err)

def rnp_encrypt_and_sign_file(src, dst, recipients, encrpswd, signers, signpswd,
                              aead=None, cipher=None, z=None, armor=False, homedir=None):
    if not homedir:
        homedir = RNPDIR
    params = ['--homedir', homedir, '--sign', '--encrypt', src, '--output', dst]
    pipe = pswd_pipe('\n'.join(encrpswd + signpswd))
    params[2:2] = ['--pass-fd', str(pipe)]

    # Encrypting passwords if any
    if encrpswd:
        params[2:2] = ['--passwords', str(len(encrpswd))]
    # Adding recipients. If list is empty then default will be used.
    for userid in reversed(recipients):
        params[2:2] = ['-r', escape_regex(userid)]
    # Adding signers. If list is empty then default will be used.
    for signer in reversed(signers):
        params[2:2] = ['-u', escape_regex(signer)]
    # Cipher or None for default
    if cipher: params[2:2] = ['--cipher', cipher]
    # Armor
    if armor: params += ['--armor']
    rnp_params_insert_aead(params, 2, aead)
    rnp_params_insert_z(params, 2, z)

    ret, _, err = run_proc(RNP, params)
    os.close(pipe)
    if ret != 0:
        raise_err('rnp encrypt-and-sign failed', err)

def rnp_decrypt_file(src, dst, password = PASSWORD, homedir = None):
    if not homedir:
        homedir = RNPDIR
    pipe = pswd_pipe(password)
    ret, out, err = run_proc(
        RNP, ['--homedir', homedir, '--pass-fd', str(pipe), '--decrypt', src, '--output', dst])
    os.close(pipe)
    if ret != 0:
        raise_err('rnp decryption failed', out + err)

def rnp_sign_file_ex(src, dst, signers, passwords, options = None):
    pipe = pswd_pipe('\n'.join(passwords))
    params = ['--homedir', RNPDIR, '--pass-fd', str(pipe), src]
    if dst: params += ['--output', dst]
    if 'cleartext' in options:
        params[4:4] = ['--clearsign']
    else:
        params[4:4] = ['--sign']
        if 'armor' in options: params += ['--armor']
        if 'detached' in options: params += ['--detach']

    for signer in reversed(signers):
        params[4:4] = ['--userid', escape_regex(signer)]

    ret, _, err = run_proc(RNP, params)
    os.close(pipe)
    if ret != 0:
        raise_err('rnp signing failed', err)


def rnp_sign_file(src, dst, signers, passwords, armor=False):
    options = []
    if armor: options += ['armor']
    rnp_sign_file_ex(src, dst, signers, passwords, options)


def rnp_sign_detached(src, signers, passwords, armor=False):
    options = ['detached']
    if armor: options += ['armor']
    rnp_sign_file_ex(src, None, signers, passwords, options)


def rnp_sign_cleartext(src, dst, signers, passwords):
    rnp_sign_file_ex(src, dst, signers, passwords, ['cleartext'])


def rnp_verify_file(src, dst, signer=None):
    params = ['--homedir', RNPDIR, '--verify-cat', src, '--output', dst]
    ret, out, err = run_proc(RNP, params)
    if ret != 0:
        raise_err('rnp verification failed', err + out)
    # Check RNP output
    match = re.match(RE_RNP_GOOD_SIGNATURE, err)
    if not match:
        raise_err('wrong rnp verification output', err)
    if signer and (match.group(1).strip() != signer.strip()):
        raise_err('rnp verification failed, wrong signer')


def rnp_verify_detached(sig, signer=None):
    ret, out, err = run_proc(RNP, ['--homedir', RNPDIR, '--verify', sig])
    if ret != 0:
        raise_err('rnp detached verification failed', err + out)
    # Check RNP output
    match = re.match(RE_RNP_GOOD_SIGNATURE, err)
    if not match:
        raise_err('wrong rnp detached verification output', err)
    if signer and (match.group(1).strip() != signer.strip()):
        raise_err('rnp detached verification failed, wrong signer'.format())


def rnp_verify_cleartext(src, signer=None):
    params = ['--homedir', RNPDIR, '--verify', src]
    ret, out, err = run_proc(RNP, params)
    if ret != 0:
        raise_err('rnp verification failed', err + out)
    # Check RNP output
    match = re.match(RE_RNP_GOOD_SIGNATURE, err)
    if not match:
        raise_err('wrong rnp verification output', err)
    if signer and (match.group(1).strip() != signer.strip()):
        raise_err('rnp verification failed, wrong signer')


def gpg_import_pubring(kpath=None):
    if not kpath:
        kpath = os.path.join(RNPDIR, PUBRING)
    ret, _, err = run_proc(
        GPG, ['--display-charset', CONSOLE_ENCODING, '--batch', '--homedir', GPGHOME, '--import', kpath])
    if ret != 0:
        raise_err(GPG_IMPORT_FAILED, err)


def gpg_import_secring(kpath=None, password = PASSWORD):
    if not kpath:
        kpath = os.path.join(RNPDIR, SECRING)
    ret, _, err = run_proc(
        GPG, ['--display-charset', CONSOLE_ENCODING, '--batch', '--passphrase', password, '--homedir', GPGHOME, '--import', kpath])
    if ret != 0:
        raise_err('gpg secret key import failed', err)


def gpg_export_secret_key(userid, password, keyfile):
    ret, _, err = run_proc(GPG, ['--batch', '--homedir', GPGHOME, GPG_LOOPBACK,
                                 '--yes', '--passphrase', password, '--output',
                                 path_for_gpg(keyfile), '--export-secret-key', userid])

    if ret != 0:
        raise_err('gpg secret key export failed', err)

def gpg_params_insert_z(params, pos, z):
    if z:
        if len(z) > 0 and z[0] != None:
            params[pos:pos] = ['--compress-algo', RNP_TO_GPG_ZALGS[z[0]]]
        if len(z) > 1 and z[1] != None:
            params[pos:pos] = ['-z', str(z[1])]

def gpg_encrypt_file(src, dst, cipher=None, z=None, armor=False):
    src = path_for_gpg(src)
    dst = path_for_gpg(dst)
    params = ['--homedir', GPGHOME, '-e', '-r', KEY_ENCRYPT, '--batch',
              '--trust-model', 'always', '--output', dst, src]
    if z: gpg_params_insert_z(params, 3, z)
    if cipher: params[3:3] = ['--cipher-algo', RNP_TO_GPG_CIPHERS[cipher]]
    if armor: params[2:2] = ['--armor']
    if GPG_NO_OLD: params[2:2] = ['--allow-old-cipher-algos']

    ret, _, err = run_proc(GPG, params)
    if ret != 0:
        raise_err('gpg encryption failed for cipher ' + cipher, err)

def gpg_symencrypt_file(src, dst, cipher=None, z=None, armor=False, aead=None):
    src = path_for_gpg(src)
    dst = path_for_gpg(dst)
    params = ['--homedir', GPGHOME, '-c', '--s2k-count', '65536', '--batch',
              '--passphrase', PASSWORD, '--output', dst, src]
    if z: gpg_params_insert_z(params, 3, z)
    if cipher: params[3:3] = ['--cipher-algo', RNP_TO_GPG_CIPHERS[cipher]]
    if GPG_NO_OLD: params[3:3] = ['--allow-old-cipher-algos']
    if armor: params[2:2] = ['--armor']
    if aead != None:
        if len(aead) > 0 and aead[0] != None:
            params[3:3] = ['--aead-algo', aead[0]]
        if len(aead) > 1 and aead[1] != None:
            params[3:3] = ['--chunk-size', str(aead[1] + 6)]
        params[3:3] = ['--rfc4880bis', '--force-aead']

    ret, _, err = run_proc(GPG, params)
    if ret != 0:
        raise_err('gpg symmetric encryption failed for cipher ' + cipher, err)


def gpg_decrypt_file(src, dst, keypass):
    src = path_for_gpg(src)
    dst = path_for_gpg(dst)
    ret, _, err = run_proc(GPG, ['--display-charset', CONSOLE_ENCODING, '--homedir', GPGHOME, GPG_LOOPBACK, '--batch',
                                   '--yes', '--passphrase', keypass, '--trust-model',
                                   'always', '-o', dst, '-d', src])
    if ret != 0:
        raise_err('gpg decryption failed', err)


def gpg_verify_file(src, dst, signer=None):
    src = path_for_gpg(src)
    dst = path_for_gpg(dst)
    ret, _, err = run_proc(GPG, ['--display-charset', CONSOLE_ENCODING, '--homedir', GPGHOME, '--batch',
                                   '--yes', '--trust-model', 'always', '-o', dst, '--verify', src])
    if ret != 0:
        raise_err('gpg verification failed', err)
    # Check GPG output
    match = re.match(RE_GPG_GOOD_SIGNATURE, err)
    if not match:
        raise_err('wrong gpg verification output', err)
    if signer and (match.group(1) != signer):
        raise_err('gpg verification failed, wrong signer')


def gpg_verify_detached(src, sig, signer=None):
    src = path_for_gpg(src)
    sig = path_for_gpg(sig)
    ret, _, err = run_proc(GPG, ['--display-charset', CONSOLE_ENCODING, '--homedir', GPGHOME, '--batch', '--yes', '--trust-model',
                                 'always', '--verify', sig, src])
    if ret != 0:
        raise_err('gpg detached verification failed', err)
    # Check GPG output
    match = re.match(RE_GPG_GOOD_SIGNATURE, err)
    if not match:
        raise_err('wrong gpg detached verification output', err)
    if signer and (match.group(1) != signer):
        raise_err('gpg detached verification failed, wrong signer')


def gpg_verify_cleartext(src, signer=None):
    src = path_for_gpg(src)
    ret, _, err = run_proc(
        GPG, ['--display-charset', CONSOLE_ENCODING, '--homedir', GPGHOME, '--batch', '--yes', '--trust-model', 'always', '--verify', src])
    if ret != 0:
        raise_err('gpg cleartext verification failed', err)
    # Check GPG output
    match = re.match(RE_GPG_GOOD_SIGNATURE, err)
    if not match:
        raise_err('wrong gpg verification output', err)
    if signer and (match.group(1) != signer):
        raise_err('gpg verification failed, wrong signer')


def gpg_sign_file(src, dst, signer, z=None, armor=False):
    src = path_for_gpg(src)
    dst = path_for_gpg(dst)
    params = ['--homedir', GPGHOME, GPG_LOOPBACK, '--batch', '--yes',
              '--passphrase', PASSWORD, '--trust-model', 'always', '-u', signer, '-o',
              dst, '-s', src]
    if z: gpg_params_insert_z(params, 3, z)
    if armor: params.insert(2, '--armor')
    ret, _, err = run_proc(GPG, params)
    if ret != 0:
        raise_err('gpg signing failed', err)


def gpg_sign_detached(src, signer, armor=False, textsig=False):
    src = path_for_gpg(src)
    params = ['--homedir', GPGHOME, GPG_LOOPBACK, '--batch', '--yes',
              '--passphrase', PASSWORD, '--trust-model', 'always', '-u', signer,
              '--detach-sign', src]
    if armor: params.insert(2, '--armor')
    if textsig: params.insert(2, '--text')
    ret, _, err = run_proc(GPG, params)
    if ret != 0:
        raise_err('gpg detached signing failed', err)


def gpg_sign_cleartext(src, dst, signer):
    src = path_for_gpg(src)
    dst = path_for_gpg(dst)
    params = ['--homedir', GPGHOME, GPG_LOOPBACK, '--batch', '--yes', '--passphrase',
              PASSWORD, '--trust-model', 'always', '-u', signer, '-o', dst, '--clearsign', src]
    ret, _, err = run_proc(GPG, params)
    if ret != 0:
        raise_err('gpg cleartext signing failed', err)


def gpg_agent_clear_cache():
    run_proc(GPGCONF, ['--homedir', GPGHOME, '--kill', 'gpg-agent'])

'''
    Things to try here later on:
    - different symmetric algorithms
    - different file sizes (block len/packet len tests)
    - different public key algorithms
    - different compression levels/algorithms
'''


def gpg_to_rnp_encryption(filesize, cipher=None, z=None):
    '''
    Encrypts with GPG and decrypts with RNP
    '''
    src, dst, dec = reg_workfiles('cleartext', '.txt', '.gpg', '.rnp')
    # Generate random file of required size
    random_text(src, filesize)
    for armor in [False, True]:
        # Encrypt cleartext file with GPG
        gpg_encrypt_file(src, dst, cipher, z, armor)
        # Decrypt encrypted file with RNP
        rnp_decrypt_file(dst, dec)
        compare_files(src, dec, RNP_DATA_DIFFERS)
        remove_files(dst, dec)
    clear_workfiles()


def file_encryption_rnp_to_gpg(filesize, z=None):
    '''
    Encrypts with RNP and decrypts with GPG and RNP
    '''
    # TODO: Would be better to do "with reg_workfiles() as src,dst,enc ... and
    # do cleanup at the end"
    src, dst, enc = reg_workfiles('cleartext', '.txt', '.gpg', '.rnp')
    # Generate random file of required size
    random_text(src, filesize)
    for armor in [False, True]:
        # Encrypt cleartext file with RNP
        rnp_encrypt_file_ex(src, enc, [KEY_ENCRYPT], None, None, None, z, armor)
        # Decrypt encrypted file with GPG
        gpg_decrypt_file(enc, dst, PASSWORD)
        compare_files(src, dst, GPG_DATA_DIFFERS)
        remove_files(dst)
        # Decrypt encrypted file with RNP
        rnp_decrypt_file(enc, dst)
        compare_files(src, dst, RNP_DATA_DIFFERS)
        remove_files(enc, dst)
    clear_workfiles()

'''
    Things to try later:
    - different public key algorithms
    - decryption with generated by GPG and imported keys
'''


def rnp_sym_encryption_gpg_to_rnp(filesize, cipher = None, z = None):
    src, dst, dec = reg_workfiles('cleartext', '.txt', '.gpg', '.rnp')
    # Generate random file of required size
    random_text(src, filesize)
    for armor in [False, True]:
        # Encrypt cleartext file with GPG
        gpg_symencrypt_file(src, dst, cipher, z, armor)
        # Decrypt encrypted file with RNP
        rnp_decrypt_file(dst, dec)
        compare_files(src, dec, RNP_DATA_DIFFERS)
        remove_files(dst, dec)
    clear_workfiles()


def rnp_sym_encryption_rnp_to_gpg(filesize, cipher = None, z = None, s2k_iter = False, s2k_msec = False):
    src, dst, enc = reg_workfiles('cleartext', '.txt', '.gpg', '.rnp')
    # Generate random file of required size
    random_text(src, filesize)
    for armor in [False, True]:
        # Encrypt cleartext file with RNP
        rnp_encrypt_file_ex(src, enc, None, [PASSWORD], None, cipher, z, armor, s2k_iter, s2k_msec)
        # Decrypt encrypted file with GPG
        gpg_decrypt_file(enc, dst, PASSWORD)
        compare_files(src, dst, GPG_DATA_DIFFERS)
        remove_files(dst)
        # Decrypt encrypted file with RNP
        rnp_decrypt_file(enc, dst)
        compare_files(src, dst, RNP_DATA_DIFFERS)
        remove_files(enc, dst)
    clear_workfiles()

def rnp_sym_encryption_rnp_aead(filesize, cipher = None, z = None, aead = None, usegpg = False):
    src, dst, enc = reg_workfiles('cleartext', '.txt', '.rnp', '.enc')
    # Generate random file of required size
    random_text(src, filesize)
    # Encrypt cleartext file with RNP
    rnp_encrypt_file_ex(src, enc, None, [PASSWORD], aead, cipher, z)
    # Decrypt encrypted file with RNP
    rnp_decrypt_file(enc, dst)
    compare_files(src, dst, RNP_DATA_DIFFERS)
    remove_files(dst)

    if usegpg:
        # Decrypt encrypted file with GPG
        gpg_decrypt_file(enc, dst, PASSWORD)
        compare_files(src, dst, GPG_DATA_DIFFERS)
        remove_files(dst, enc)
        # Encrypt cleartext file with GPG
        gpg_symencrypt_file(src, enc, cipher, z, False, aead)
        # Decrypt encrypted file with RNP
        rnp_decrypt_file(enc, dst)
        compare_files(src, dst, RNP_DATA_DIFFERS)

    clear_workfiles()

def rnp_signing_rnp_to_gpg(filesize):
    src, sig, ver = reg_workfiles('cleartext', '.txt', '.sig', '.ver')
    # Generate random file of required size
    random_text(src, filesize)
    for armor in [False, True]:
        # Sign file with RNP
        rnp_sign_file(src, sig, [KEY_SIGN_RNP], [PASSWORD], armor)
        # Verify signed file with RNP
        rnp_verify_file(sig, ver, KEY_SIGN_RNP)
        compare_files(src, ver, 'rnp verified data differs')
        remove_files(ver)
        # Verify signed message with GPG
        gpg_verify_file(sig, ver, KEY_SIGN_RNP)
        compare_files(src, ver, 'gpg verified data differs')
        remove_files(sig, ver)
    clear_workfiles()


def rnp_detached_signing_rnp_to_gpg(filesize):
    src, sig, asc = reg_workfiles('cleartext', '.txt', EXT_SIG, EXT_ASC)
    # Generate random file of required size
    random_text(src, filesize)
    for armor in [True, False]:
        # Sign file with RNP
        rnp_sign_detached(src, [KEY_SIGN_RNP], [PASSWORD], armor)
        sigpath = asc if armor else sig
        # Verify signature with RNP
        rnp_verify_detached(sigpath, KEY_SIGN_RNP)
        # Verify signed message with GPG
        gpg_verify_detached(src, sigpath, KEY_SIGN_RNP)
        remove_files(sigpath)
    clear_workfiles()


def rnp_cleartext_signing_rnp_to_gpg(filesize):
    src, asc = reg_workfiles('cleartext', '.txt', EXT_ASC)
    # Generate random file of required size
    random_text(src, filesize)
    # Sign file with RNP
    rnp_sign_cleartext(src, asc, [KEY_SIGN_RNP], [PASSWORD])
    # Verify signature with RNP
    rnp_verify_cleartext(asc, KEY_SIGN_RNP)
    # Verify signed message with GPG
    gpg_verify_cleartext(asc, KEY_SIGN_RNP)
    clear_workfiles()


def rnp_signing_gpg_to_rnp(filesize, z=None):
    src, sig, ver = reg_workfiles('cleartext', '.txt', '.sig', '.ver')
    # Generate random file of required size
    random_text(src, filesize)
    for armor in [True, False]:
        # Sign file with GPG
        gpg_sign_file(src, sig, KEY_SIGN_GPG, z, armor)
        # Verify file with RNP
        rnp_verify_file(sig, ver, KEY_SIGN_GPG)
        compare_files(src, ver, 'rnp verified data differs')
        remove_files(sig, ver)
    clear_workfiles()


def rnp_detached_signing_gpg_to_rnp(filesize, textsig=False):
    src, sig, asc = reg_workfiles('cleartext', '.txt', EXT_SIG, EXT_ASC)
    # Generate random file of required size
    random_text(src, filesize)
    for armor in [True, False]:
        # Sign file with GPG
        gpg_sign_detached(src, KEY_SIGN_GPG, armor, textsig)
        sigpath = asc if armor else sig
        # Verify file with RNP
        rnp_verify_detached(sigpath, KEY_SIGN_GPG)
    clear_workfiles()

def rnp_cleartext_signing_gpg_to_rnp(filesize):
    src, asc = reg_workfiles('cleartext', '.txt', EXT_ASC)
    # Generate random file of required size
    random_text(src, filesize)
    # Sign file with GPG
    gpg_sign_cleartext(src, asc, KEY_SIGN_GPG)
    # Verify signature with RNP
    rnp_verify_cleartext(asc, KEY_SIGN_GPG)
    # Verify signed message with GPG
    gpg_verify_cleartext(asc, KEY_SIGN_GPG)
    clear_workfiles()

def gpg_check_features():
    global GPG_AEAD, GPG_AEAD_EAX, GPG_AEAD_OCB, GPG_NO_OLD, GPG_BRAINPOOL
    _, out, _ = run_proc(GPG, ["--version"])
    # AEAD
    GPG_AEAD_EAX = re.match(r'(?s)^.*AEAD:.*EAX.*', out) is not None
    GPG_AEAD_OCB = re.match(r'(?s)^.*AEAD:.*OCB.*', out) is not None
    # Version 2.3.0-beta1598 and up drops support of 64-bit block algos
    match = re.match(r'(?s)^.*gpg \(GnuPG\) (\d+)\.(\d+)\.(\d+)(-beta(\d+))?.*$', out)
    if not match:
        raise_err('Failed to parse GnuPG version.')
    ver = [int(match.group(1)), int(match.group(2)), int(match.group(3))]
    beta = int(match.group(5)) if match.group(5) else 0
    if not beta:
        GPG_NO_OLD = ver >= [2, 3, 0]
    else:
        GPG_NO_OLD = ver == [2, 3, 0] and (beta >= 1598)
    # Version 2.4.0 and up doesn't support EAX and doesn't has AEAD in output
    if ver >= [2, 4, 0]:
        GPG_AEAD_OCB = True
        GPG_AEAD_EAX = False
    GPG_AEAD = GPG_AEAD_OCB or GPG_AEAD_EAX
    # Check whether Brainpool curves are supported
    _, out, _ = run_proc(GPG, ["--with-colons", "--list-config", "curve"])
    GPG_BRAINPOOL = re.match(r'(?s)^.*brainpoolP256r1.*', out) is not None
    print('GPG_AEAD_EAX: ' + str(GPG_AEAD_EAX))
    print('GPG_AEAD_OCB: ' + str(GPG_AEAD_OCB))
    print('GPG_NO_OLD: ' + str(GPG_NO_OLD))
    print('GPG_BRAINPOOL: ' + str(GPG_BRAINPOOL))

def rnp_check_features():
    global RNP_TWOFISH, RNP_BRAINPOOL, RNP_AEAD, RNP_AEAD_EAX, RNP_AEAD_OCB, RNP_AEAD_OCB_AES, RNP_IDEA, RNP_BLOWFISH, RNP_CAST5, RNP_RIPEMD160, RNP_PQC, RNP_SM2
    global RNP_BOTAN_OCB_AV
    global RNP_BACKEND
    ret, out, _ = run_proc(RNP, ['--version'])
    if ret != 0:
        raise_err('Failed to get RNP version.')
    # AEAD
    RNP_AEAD_EAX = re.match(r'(?s)^.*AEAD:.*EAX.*', out) is not None
    RNP_AEAD_OCB = re.match(r'(?s)^.*AEAD:.*OCB.*', out) is not None
    RNP_AEAD = RNP_AEAD_EAX or RNP_AEAD_OCB
    RNP_AEAD_OCB_AES = RNP_AEAD_OCB and re.match(r'(?s)^.*Backend.*OpenSSL.*', out) is not None
    # OpenSSL backend
    if re.match(r'(?s)^.*Backend.*OpenSSL.*', out):
        RNP_BACKEND = 'openssl'
    # Botan OCB crash
    if re.match(r'(?s)^.*Backend.*Botan.*', out):
        RNP_BACKEND = 'botan'
        match = re.match(r'(?s)^.*Backend version: ([\d]+)\.([\d]+)\.([\d]+).*$', out)
        ver = [int(match.group(1)), int(match.group(2)), int(match.group(3))]
        if ver <= [2, 19, 3]:
            RNP_BOTAN_OCB_AV = True
        if (ver >= [3, 0, 0]) and (ver <= [3, 2, 0]):
            RNP_BOTAN_OCB_AV = True
    if not RNP_BACKEND:
        raise_err('Failed to detect backend!')
    # Twofish
    RNP_TWOFISH = re.match(r'(?s)^.*Encryption:.*TWOFISH.*', out) is not None
    # Brainpool curves
    RNP_BRAINPOOL = re.match(r'(?s)^.*Curves:.*brainpoolP256r1.*brainpoolP384r1.*brainpoolP512r1.*', out) is not None
    # IDEA encryption algorithm
    RNP_IDEA = re.match(r'(?s)^.*Encryption:.*IDEA.*', out) is not None
    RNP_BLOWFISH = re.match(r'(?s)^.*Encryption:.*BLOWFISH.*', out) is not None
    RNP_CAST5 = re.match(r'(?s)^.*Encryption:.*CAST5.*', out) is not None
    RNP_RIPEMD160 = re.match(r'(?s)^.*Hash:.*RIPEMD160.*', out) is not None
    # SM2
    RNP_SM2 = re.match(r'(?s)^.*Public key:.*SM2.*', out) is not None
    # Determine PQC support in general. If present, assume that all PQC schemes are supported.
    pqc_strs = ['ML-KEM', 'ML-DSA']
    RNP_PQC = any([re.match('(?s)^.*Public key:.*' + scheme + '.*', out) is not None for scheme in pqc_strs])
    print('RNP_TWOFISH: ' + str(RNP_TWOFISH))
    print('RNP_BLOWFISH: ' + str(RNP_BLOWFISH))
    print('RNP_IDEA: ' + str(RNP_IDEA))
    print('RNP_CAST5: ' + str(RNP_CAST5))
    print('RNP_RIPEMD160: ' + str(RNP_RIPEMD160))
    print('RNP_BRAINPOOL: ' + str(RNP_BRAINPOOL))
    print('RNP_AEAD_EAX: ' + str(RNP_AEAD_EAX))
    print('RNP_AEAD_OCB: ' + str(RNP_AEAD_OCB))
    print('RNP_AEAD_OCB_AES: ' + str(RNP_AEAD_OCB_AES))
    print('RNP_BOTAN_OCB_AV: ' + str(RNP_BOTAN_OCB_AV))
    print('RNP_PQC: ' + str(RNP_PQC))

def setup(loglvl):
    # Setting up directories.
    global RMWORKDIR, WORKDIR, RNPDIR, RNPDIR2, RNP, RNPK, GPG, GPGDIR, GPGHOME, GPGCONF
    logging.basicConfig(stream=sys.stderr, format="%(message)s")
    logging.getLogger().setLevel(loglvl)
    WORKDIR = tempfile.mkdtemp(prefix='rnpctmp')
    set_workdir(WORKDIR)
    RMWORKDIR = True

    logging.info('Running in ' + WORKDIR)

    RNPDIR = os.path.join(WORKDIR, '.rnp')
    RNPDIR2 = RNPDIR + '2'
    RNP = os.getenv('RNP_TESTS_RNP_PATH') or 'rnp'
    RNPK = os.getenv('RNP_TESTS_RNPKEYS_PATH') or 'rnpkeys'
    shutil.rmtree(RNPDIR, ignore_errors=True)
    os.mkdir(RNPDIR, 0o700)

    os.environ["RNP_LOG_CONSOLE"] = "1"

    GPGDIR = os.path.join(WORKDIR, '.gpg')
    GPGHOME = path_for_gpg(GPGDIR) if is_windows() else GPGDIR
    GPG = os.getenv('RNP_TESTS_GPG_PATH') or find_utility('gpg')
    GPGCONF = os.getenv('RNP_TESTS_GPGCONF_PATH') or find_utility('gpgconf')
    gpg_check_features()
    rnp_check_features()
    shutil.rmtree(GPGDIR, ignore_errors=True)
    os.mkdir(GPGDIR, 0o700)

def data_path(subpath):
    ''' Constructs path to the tests data file/dir'''
    return os.path.join(os.path.dirname(os.path.realpath(__file__)), 'data', subpath)

def key_path(file_base_name, secret):
    ''' Constructs path to the .gpg file'''
    path=os.path.join(os.path.dirname(os.path.realpath(__file__)), 'data/cli_EncryptSign',
                      file_base_name)
    return ''.join([path, '-sec' if secret else '', '.gpg'])

def rnp_supported_ciphers(aead = False):
    ciphers = ['AES', 'AES192', 'AES256']
    if aead and RNP_AEAD_OCB_AES:
        return ciphers
    ciphers += ['CAMELLIA128', 'CAMELLIA192', 'CAMELLIA256']
    if RNP_TWOFISH:
        ciphers += ['TWOFISH']
    # AEAD supports only 128-bit block ciphers
    if aead:
        return ciphers
    ciphers += ['3DES']
    if RNP_IDEA:
        ciphers += ['IDEA']
    if RNP_BLOWFISH:
        ciphers += ['BLOWFISH']
    if RNP_CAST5:
        ciphers += ['CAST5']
    return ciphers

class TestIdMixin(object):

    @property
    def test_id(self):
        return "".join(self.id().split('.')[1:3])

class KeyLocationChooserMixin(object):
    def __init__(self):
        # If set it will try to import a key from provided location
        # otherwise it will try to generate a key
        self.__op_key_location = None
        self.__op_key_gen_cmd = None

    @property
    def operation_key_location(self):
        return self.__op_key_location

    @operation_key_location.setter
    def operation_key_location(self, key):
        if (type(key) is not tuple): raise RuntimeError("Key must be tuple(pub,sec)")
        self.__op_key_location = key
        self.__op_key_gen_cmd = None

    @property
    def operation_key_gencmd(self):
        return self.__op_key_gen_cmd

    @operation_key_gencmd.setter
    def operation_key_gencmd(self, cmd):
        self.__op_key_gen_cmd = cmd
        self.__op_key_location = None

'''
    Things to try here later on:
    - different public key algorithms
    - different key protection levels/algorithms
    - armored import/export
'''
class Keystore(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        clear_keyrings()

    @classmethod
    def tearDownClass(cls):
        clear_keyrings()

    def tearDown(self):
        clear_workfiles()

    def _rnpkey_generate_rsa(self, bits= None):
        # Setup command line params
        if bits:
            params = ['--numbits', str(bits)]
        else:
            params = []
            bits = 3072

        userid = str(bits) + '@rnptest'
        # Open pipe for password
        pipe = pswd_pipe(PASSWORD)
        params = params + ['--homedir', RNPDIR, '--pass-fd', str(pipe),
                           '--userid', userid, '--s2k-iterations', '50000', '--generate-key']
        # Run key generation
        ret, _, _ = run_proc(RNPK, params)
        os.close(pipe)
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Check packets using the gpg
        match = check_packets(os.path.join(RNPDIR, PUBRING), RE_RSA_KEY)
        self.assertTrue(match, 'generated key check failed')
        keybits = int(match.group(1))
        self.assertLessEqual(keybits, bits, 'too much bits')
        self.assertGreater(keybits, bits - 8, 'too few bits')
        keyid = match.group(2)
        self.assertEqual(match.group(3), userid, 'wrong user id')
        # List keys using the rnpkeys
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0, KEY_LIST_FAILED)
        match = re.match(RE_RSA_KEY_LIST, out)
        # Compare key ids
        self.assertTrue(match, 'wrong RSA key list output')
        self.assertEqual(match.group(3)[-16:], match.group(2), 'wrong fp')
        self.assertEqual(match.group(2), keyid.lower(), 'wrong keyid')
        self.assertEqual(match.group(1), str(bits), 'wrong key bits in list')
        # Import key to the gnupg
        ret, _, _ = run_proc(GPG, ['--batch', '--passphrase', PASSWORD, '--homedir',
                                       GPGHOME, '--import',
                                       path_for_gpg(os.path.join(RNPDIR, PUBRING)),
                                       path_for_gpg(os.path.join(RNPDIR, SECRING))])
        self.assertEqual(ret, 0, GPG_IMPORT_FAILED)
        # Cleanup and return
        clear_keyrings()

    def test_generate_default_rsa_key(self):
        self._rnpkey_generate_rsa()

    def test_rnpkeys_keygen_invalid_parameters(self):
        # Pass invalid numbits
        ret, _, err = run_proc(RNPK, ['--numbits', 'wrong', '--homedir', RNPDIR, '--password', 'password',
                                      '--userid', 'wrong', '--generate-key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*wrong bits value: wrong.*')
        # Too small
        ret, _, err = run_proc(RNPK, ['--numbits', '768', '--homedir', RNPDIR, '--password', 'password',
                                      '--userid', '768', '--generate-key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*wrong bits value: 768.*')
        # Wrong hash algorithm
        ret, _, err = run_proc(RNPK, ['--hash', 'BAD_HASH', '--homedir', RNPDIR, '--password', 'password',
                                      '--userid', 'bad_hash', '--generate-key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Unsupported hash algorithm: BAD_HASH.*')
        # Wrong S2K iterations
        ret, _, err = run_proc(RNPK, ['--s2k-iterations', 'WRONG_ITER', '--homedir', RNPDIR, '--password', 'password',
                                      '--userid', 'wrong_iter', '--generate-key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Wrong iterations value: WRONG_ITER.*')
        # Wrong S2K msec
        ret, _, err = run_proc(RNPK, ['--s2k-msec', 'WRONG_MSEC', '--homedir', RNPDIR, '--password', 'password',
                                      '--userid', 'wrong_msec', '--generate-key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Invalid s2k msec value: WRONG_MSEC.*')
        # Wrong cipher
        ret, _, err = run_proc(RNPK, ['--cipher', 'WRONG_AES', '--homedir', RNPDIR, '--password', 'password',
                                      '--userid', 'wrong_aes', '--generate-key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Unsupported encryption algorithm: WRONG_AES.*Failed to process argument --cipher.*')

    def test_generate_multiple_rsa_key__check_if_available(self):
        '''
        Generate multiple RSA keys and check if they are all available
        '''
        clear_keyrings()
        # Generate 5 keys with different user ids
        for i in range(0, 5):
            # generate the next key
            pipe = pswd_pipe(PASSWORD)
            userid = str(i) + '@rnp-multiple'
            ret, _, _ = run_proc(RNPK, ['--numbits', '2048', '--homedir', RNPDIR, '--s2k-msec', '100',
                                        '--cipher', 'AES-128', '--pass-fd', str(pipe), '--userid', userid,
                                        '--generate-key'])
            os.close(pipe)
            self.assertEqual(ret, 0, KEY_GEN_FAILED)
            # list keys using the rnpkeys, checking whether it reports correct key
            # number
            ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
            self.assertEqual(ret, 0, KEY_LIST_FAILED)
            match = re.match(RE_MULTIPLE_KEY_LIST, out)
            self.assertTrue(match, KEY_LIST_WRONG)
            self.assertEqual(match.group(1), str((i + 1) * 2), 'wrong key count')

        # Checking the 5 keys output
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0, KEY_LIST_FAILED)
        self.assertRegex(out, RE_MULTIPLE_KEY_5, KEY_LIST_WRONG)

        # Cleanup and return
        clear_keyrings()

    def test_generate_key_with_gpg_import_to_rnp(self):
        '''
        Generate key with GnuPG and import it to rnp
        '''
        # Generate key in GnuPG
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--passphrase',
                                       '', '--quick-generate-key', 'rsakey@gpg', 'rsa'])
        self.assertEqual(ret, 0, 'gpg key generation failed')
        # Getting fingerprint of the generated key
        ret, out, err = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--list-keys'])
        match = re.match(RE_GPG_SINGLE_RSA_KEY, out)
        self.assertTrue(match, 'wrong gpg key list output')
        keyfp = match.group(1)
        # Exporting generated public key
        ret, out, err = run_proc(
            GPG, ['--batch', '--homedir', GPGHOME, '--armor', '--export', keyfp])
        self.assertEqual(ret, 0, 'gpg : public key export failed')
        pubpath = os.path.join(RNPDIR, keyfp + '-pub.asc')
        with open(pubpath, 'w+') as f:
            f.write(out)
        # Exporting generated secret key
        ret, out, err = run_proc(
            GPG, ['--batch', '--homedir', GPGHOME, '--armor', '--export-secret-key', keyfp])
        self.assertEqual(ret, 0, 'gpg : secret key export failed')
        secpath = os.path.join(RNPDIR, keyfp + '-sec.asc')
        with open(secpath, 'w+') as f:
            f.write(out)
        # Importing public key to rnp
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', pubpath])
        self.assertEqual(ret, 0, 'rnp : public key import failed')
        # Importing secret key to rnp
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', secpath])
        self.assertEqual(ret, 0, 'rnp : secret key import failed')

    def test_generate_with_rnp_import_to_gpg(self):
        '''
        Generate key with RNP and export it and then import to GnuPG
        '''
        # Open pipe for password
        pipe = pswd_pipe(PASSWORD)
        # Run key generation
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--pass-fd', str(pipe),
                                        '--userid', 'rsakey@rnp', '--generate-key'])
        os.close(pipe)
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Export key
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'rsakey@rnp'])
        self.assertEqual(ret, 0, 'key export failed')
        pubpath = os.path.join(RNPDIR, 'rnpkey-pub.asc')
        with open(pubpath, 'w+') as f:
            f.write(out)
        # Import key with GPG
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import',
                                       path_for_gpg(pubpath)])
        self.assertEqual(ret, 0, 'gpg : public key import failed')

    def test_generate_to_kbx(self):
        '''
        Generate KBX with RNP and ensurethat the key can be read with GnuPG
        '''
        clear_keyrings()
        pipe = pswd_pipe(PASSWORD)
        kbx_userid_tracker = 'kbx_userid_tracker@rnp'
        # Run key generation
        ret, out, _ = run_proc(RNPK, ['--gen-key', '--keystore-format', 'GPG21',
                                        '--userid', kbx_userid_tracker, '--homedir',
                                        RNPDIR, '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Read KBX with GPG
        ret, out, _ = run_proc(GPG, ['--homedir', path_for_gpg(RNPDIR), '--list-keys'])
        self.assertEqual(ret, 0, 'gpg : failed to read KBX')
        self.assertIn(kbx_userid_tracker, out, 'gpg : failed to read expected key from KBX')
        clear_keyrings()

    def test_generate_protection_pass_fd(self):
        '''
        Generate key with RNP, using the --pass-fd parameter, and make sure key is encrypted
        '''
        clear_keyrings()
        # Open pipe for password
        pipe = pswd_pipe(PASSWORD)
        # Run key generation
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--pass-fd', str(pipe),
                                        '--userid', KEY_ENC_RNP, '--generate-key'])
        os.close(pipe)
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Check packets using the gpg
        params = ['--homedir', RNPDIR, '--list-packets', os.path.join(RNPDIR, SECRING)]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, RE_RNP_ENCRYPTED_KEY, 'wrong encrypted secret key listing')

    def test_generate_protection_password(self):
        '''
        Generate key with RNP, using the --password parameter, and make sure key is encrypted
        '''
        clear_keyrings()
        params = ['--homedir', RNPDIR, '--password', 'password', '--userid', KEY_ENC_RNP, '--generate-key']
        ret, _, _ = run_proc(RNPK, params)
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Check packets using the gpg
        params = ['--homedir', RNPDIR, '--list-packets', os.path.join(RNPDIR, SECRING)]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, RE_RNP_ENCRYPTED_KEY, 'wrong encrypted secret key listing')

    def test_generate_unprotected_key(self):
        '''
        Generate key with RNP, using the --password parameter, and make sure key is encrypted
        '''
        clear_keyrings()
        params = ['--homedir', RNPDIR, '--password=', '--userid', KEY_ENC_RNP, '--generate-key']
        ret, _, _ = run_proc(RNPK, params)
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Check packets using the gpg
        params = ['--homedir', RNPDIR, '--list-packets', os.path.join(RNPDIR, SECRING)]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertNotRegex(out, RE_RNP_ENCRYPTED_KEY, 'wrong unprotected secret key listing')

    def test_generate_preferences(self):
        pipe = pswd_pipe(PASSWORD)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--pass-fd', str(pipe), '--userid',
                                      'eddsa_25519_prefs', '--generate-key', '--expert'], '22\n')
        os.close(pipe)
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(RNP, ['--list-packets', os.path.join(RNPDIR, PUBRING)])
        self.assertRegex(out, r'.*preferred symmetric algorithms: AES-256, AES-192, AES-128 \(9, 8, 7\).*')
        self.assertRegex(out, r'.*preferred hash algorithms: SHA256, SHA384, SHA512, SHA224 \(8, 9, 10, 11\).*')

    def test_import_signatures(self):
        clear_keyrings()
        RE_SIG_2_UNCHANGED = r'(?s)^.*Import finished: 0 new signatures, 2 unchanged, 0 unknown.*'
        # Import command without the path parameter
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-sigs'])
        self.assertNotEqual(ret, 0, 'Sigs import without file failed')
        self.assertRegex(err, r'(?s)^.*Import path isn\'t specified.*', 'Sigs import without file wrong output')
        # Import command with invalid path parameter
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-sigs', data_path('test_key_validity/alice-rev-no-file.pgp')])
        self.assertNotEqual(ret, 0, 'Sigs import with invalid path failed')
        self.assertRegex(err, r'(?s)^.*Failed to create input for .*', 'Sigs import with invalid path wrong output')
        # Try to import signature to empty keyring
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-sigs', data_path('test_key_validity/alice-rev.pgp')])
        self.assertEqual(ret, 0, 'Alice key rev import failed')
        self.assertRegex(err, r'(?s)^.*Import finished: 0 new signatures, 0 unchanged, 1 unknown.*', 'Alice key rev import wrong output')
        # Import Basil's key
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_validity/basil-pub.asc')])
        self.assertEqual(ret, 0, 'Basil key import failed')
        # Try to import Alice's signatures with Basil's key only
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_validity/alice-sigs.pgp')])
        self.assertEqual(ret, 0, 'Alice sigs import failed')
        self.assertRegex(err, r'(?s)^.*Import finished: 0 new signatures, 0 unchanged, 2 unknown.*', 'Alice sigs import wrong output')
        # Import Alice's key without revocation/direct-key signatures
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_PUB)])
        self.assertEqual(ret, 0, ALICE_IMPORT_FAIL)
        # Import key revocation signature
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-sigs', data_path('test_key_validity/alice-rev.pgp')])
        self.assertEqual(ret, 0, 'Alice key rev import failed')
        self.assertRegex(err, RE_SIG_1_IMPORT, 'Alice key rev import wrong output')
        # Import direct-key signature
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_validity/alice-revoker-sig.pgp')])
        self.assertEqual(ret, 0, 'Alice direct-key sig import failed')
        self.assertRegex(err, RE_SIG_1_IMPORT, 'Alice direct-key sig import wrong output')
        # Try to import two signatures again
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_validity/alice-sigs.pgp')])
        self.assertEqual(ret, 0, 'Alice sigs reimport failed')
        self.assertRegex(err, RE_SIG_2_UNCHANGED, 'Alice sigs file reimport wrong output')
        # Import two signatures again via stdin
        stext = file_text(data_path('test_key_validity/alice-sigs.asc'))
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', '-'], stext)
        self.assertEqual(ret, 0, 'Alice sigs stdin reimport failed')
        self.assertRegex(err, RE_SIG_2_UNCHANGED, 'Alice sigs stdin reimport wrong output')
        # Import two signatures via env variable
        os.environ["SIG_FILE"] = stext
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', 'env:SIG_FILE'])
        self.assertEqual(ret, 0, 'Alice sigs env reimport failed')
        self.assertRegex(err, RE_SIG_2_UNCHANGED, 'Alice sigs var reimport wrong output')
        # Try to import malformed signatures
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_validity/alice-sigs-malf.pgp')])
        self.assertNotEqual(ret, 0, 'Alice malformed sigs import failed')
        self.assertRegex(err, r'(?s)^.*Failed to import signatures from .*', 'Alice malformed sigs wrong output')

    def test_export_revocation(self):
        clear_keyrings()
        OUT_NO_REV = 'no-revocation.pgp'
        OUT_ALICE_REV = 'alice-revocation.pgp'
        # Import Alice's public key and be unable to export revocation
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_PUB)])
        self.assertEqual(ret, 0, ALICE_IMPORT_FAIL)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', 'alice'])
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*Revoker secret key not found.*', 'Wrong pubkey revocation export output')
        # Import Alice's secret key and subkey
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_SUB_SEC)])
        self.assertEqual(ret, 0, 'Alice secret key import failed')
        # Attempt to export revocation without specifying key
        pipe = pswd_pipe(PASSWORD)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*You need to specify key to generate revocation for.*', 'Wrong no key revocation export output')
        # Attempt to export revocation for unknown key
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', 'basil'])
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*Key matching \'basil\' not found.*', 'Wrong unknown key revocation export output')
        # Attempt to export revocation for subkey
        pipe = pswd_pipe(PASSWORD)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', 'DD23CEB7FEBEFF17'])
        os.close(pipe)
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*Key matching \'DD23CEB7FEBEFF17\' not found.*', 'Wrong subkey revocation export output')
        # Attempt to export revocation with too broad search
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_validity/basil-sec.asc')])
        self.assertEqual(ret, 0, 'Basil secret key import failed')
        pipe = pswd_pipe(PASSWORD)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', 'rnp', '--pass-fd', str(pipe),
                                      '--output', OUT_NO_REV, '--force'])
        os.close(pipe)
        self.assertNotEqual(ret, 0, 'Failed to fail to export revocation')
        self.assertFalse(os.path.isfile(OUT_NO_REV), 'Failed to fail to export revocation')
        self.assertRegex(err, r'(?s)^.*Ambiguous input: too many keys found for \'rnp\'.*', 'Wrong revocation export output')
        # Finally successfully export revocation
        pipe = pswd_pipe(PASSWORD)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', '0451409669FFDE3C', '--pass-fd', str(pipe),
                                    '--output', OUT_ALICE_REV, '--overwrite'])
        os.close(pipe)
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(OUT_ALICE_REV))
        with open(OUT_ALICE_REV, "rb") as armored:
            self.assertRegex(armored.read().decode('utf-8'), r'-----END PGP PUBLIC KEY BLOCK-----\r\n$', 'Armor tail not found')
        # Check revocation contents
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '--list-packets', OUT_ALICE_REV])
        self.assertEqual(ret, 0)
        self.assertNotEqual(len(out), 0)
        match = re.match(RE_RNP_REVOCATION_SIG, out)
        self.assertTrue(match, 'Wrong revocation signature contents')
        self.assertEqual(match.group(1).strip(), '0 (No reason)', 'Wrong revocation signature reason')
        self.assertEqual(match.group(2).strip(), '', 'Wrong revocation signature message')
        # Make sure it can be imported back
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-sigs', OUT_ALICE_REV])
        self.assertEqual(ret, 0, 'Failed to import revocation back')
        self.assertRegex(err, RE_SIG_1_IMPORT, 'Revocation import wrong output')
        # Make sure file will not be overwritten with --force parameter
        with open(OUT_ALICE_REV, 'w+') as f:
            f.truncate(10)
        pipe = pswd_pipe(PASSWORD)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', '0451409669FFDE3C', '--pass-fd', str(pipe), '--output', OUT_ALICE_REV, '--force', '--notty'], '\n\n')
        os.close(pipe)
        self.assertNotEqual(ret, 0, 'Revocation was overwritten with --force')
        self.assertEqual(10, os.stat(OUT_ALICE_REV).st_size, 'Revocation was overwritten with --force')
        # Make sure file will not be overwritten without --overwrite parameter
        pipe = pswd_pipe(PASSWORD)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', '0451409669FFDE3C', '--pass-fd', str(pipe), '--output', OUT_ALICE_REV, '--notty'], '\n\n')
        os.close(pipe)
        self.assertNotEqual(ret, 0, 'Revocation was overwritten without --overwrite and --force')
        self.assertTrue(os.path.isfile(OUT_ALICE_REV), 'Revocation was overwritten without --overwrite')
        self.assertEqual(10, os.stat(OUT_ALICE_REV).st_size, 'Revocation was overwritten without --overwrite')
        # Make sure file will be overwritten with --overwrite parameter
        pipe = pswd_pipe(PASSWORD)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', '0451409669FFDE3C', '--pass-fd', str(pipe), '--output', OUT_ALICE_REV, '--overwrite'])
        os.close(pipe)
        self.assertEqual(ret, 0)
        self.assertGreater(os.stat(OUT_ALICE_REV).st_size, 10)
        # Create revocation with wrong code - 'no longer valid' (which is usable only for userid)
        pipe = pswd_pipe(PASSWORD)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', 'alice', '--rev-type', 'no longer valid',
                                        '--pass-fd', str(pipe), '--output', OUT_NO_REV, '--force'])
        os.close(pipe)
        self.assertNotEqual(ret, 0, 'Failed to use wrong revocation reason')
        self.assertFalse(os.path.isfile(OUT_NO_REV))
        self.assertRegex(err, r'(?s)^.*Wrong key revocation code: 32.*', 'Wrong revocation export output')
        # Create revocation without rev-code parameter
        pipe = pswd_pipe(PASSWORD)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', 'alice', '--pass-fd', str(pipe),
                                        '--output', OUT_NO_REV, '--force', '--rev-type'])
        os.close(pipe)
        self.assertNotEqual(ret, 0, 'Failed to use rev-type without parameter')
        self.assertFalse(os.path.isfile(OUT_NO_REV), 'Failed to use rev-type without parameter')
        # Create another revocation with custom code/reason
        revcodes = {"0" : "0 (No reason)", "1" : "1 (Superseded)", "2" : "2 (Compromised)",
                    "3" : "3 (Retired)", "no" : "0 (No reason)", "superseded" : "1 (Superseded)",
                    "compromised" : "2 (Compromised)", "retired" : "3 (Retired)"}
        for revcode in revcodes:
            revreason = 'Custom reason: ' + revcode
            pipe = pswd_pipe(PASSWORD)
            ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-rev', '0451409669FFDE3C', '--pass-fd', str(pipe),
                                            '--output', OUT_ALICE_REV, '--overwrite', '--rev-type', revcode, '--rev-reason', revreason])
            os.close(pipe)
            self.assertEqual(ret, 0, 'Failed to export revocation with code ' + revcode)
            self.assertTrue(os.path.isfile(OUT_ALICE_REV), 'Failed to export revocation with code ' + revcode)
            # Check revocation contents
            with open(OUT_ALICE_REV, "rb") as armored:
                self.assertRegex(armored.read().decode('utf-8'), r'-----END PGP PUBLIC KEY BLOCK-----\r\n$', 'Armor tail not found')
            ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '--list-packets', OUT_ALICE_REV])
            self.assertEqual(ret, 0, 'Failed to list exported revocation packets')
            self.assertNotEqual(len(out), 0, 'Failed to list exported revocation packets')
            match = re.match(RE_RNP_REVOCATION_SIG, out)
            self.assertTrue(match)
            self.assertEqual(match.group(1).strip(), revcodes[revcode], 'Wrong revocation signature revcode')
            self.assertEqual(match.group(2).strip(), revreason, 'Wrong revocation signature reason')
            # Make sure it is also imported back
            ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-sigs', OUT_ALICE_REV])
            self.assertEqual(ret, 0)
            self.assertRegex(err, RE_SIG_1_IMPORT, 'Revocation import wrong output')
            # Now let's import it with GnuPG
            gpg_import_pubring(data_path(KEY_ALICE_PUB))
            ret, _, err = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import', OUT_ALICE_REV])
            self.assertEqual(ret, 0, 'gpg signature revocation import failed')
            self.assertRegex(err, RE_GPG_REVOCATION_IMPORT, 'Wrong gpg revocation import output')

        os.remove(OUT_ALICE_REV)
        clear_keyrings()

    def test_import_keys(self):
        clear_keyrings()
        KEY_BOTH=data_path('test_stream_key_merge/key-both.asc')
        # try to import non-existing file
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', data_path('thiskeyfiledoesnotexist')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Failed to create input for .*thiskeyfiledoesnotexist.*')
        # try malformed file
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', data_path('test_key_validity/alice-sigs-malf.pgp')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*failed to import key\(s\) from .*test_key_validity/alice-sigs-malf.pgp, stopping\..*')
        self.assertRegex(err, r'(?s)^.*Import finished: 0 keys processed, 0 new public keys, 0 new secret keys, 0 updated, 0 unchanged\..*')
        # try --import
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_SUB_PUB)])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, 2 new public keys, 0 new secret keys, 0 updated, 0 unchanged\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_SUB_PUB)])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, 0 new public keys, 0 new secret keys, 0 updated, 2 unchanged\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', KEY_BOTH])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 6 keys processed, 3 new public keys, 3 new secret keys, 0 updated, 0 unchanged\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', KEY_BOTH])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 6 keys processed, 0 new public keys, 0 new secret keys, 0 updated, 6 unchanged\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_validity/alice-sign-sub-exp-pub.asc')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, 1 new public keys, 0 new secret keys, 1 updated, 0 unchanged\..*')
        clear_keyrings()
        # try --import-key
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', data_path(KEY_ALICE_SUB_PUB)])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, 2 new public keys, 0 new secret keys, 0 updated, 0 unchanged\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', data_path(KEY_ALICE_SUB_PUB)])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, 0 new public keys, 0 new secret keys, 0 updated, 2 unchanged\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', KEY_BOTH])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 6 keys processed, 3 new public keys, 3 new secret keys, 0 updated, 0 unchanged\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', KEY_BOTH])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 6 keys processed, 0 new public keys, 0 new secret keys, 0 updated, 6 unchanged\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-key', data_path('test_key_validity/alice-sign-sub-exp-pub.asc')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, 1 new public keys, 0 new secret keys, 1 updated, 0 unchanged\..*')
        clear_keyrings()

    def test_export_keys(self):
        PUB_KEY = r'(?s)^.*' \
        r'-----BEGIN PGP PUBLIC KEY BLOCK-----.*' \
        r'-----END PGP PUBLIC KEY BLOCK-----.*$'
        PUB_KEY_PKTS = r'(?s)^.*' \
        r'Public key packet.*' \
        r'keyid: 0x0451409669ffde3c.*' \
        r'Public subkey packet.*' \
        r'keyid: 0xdd23ceb7febeff17.*$'
        SEC_KEY = r'(?s)^.*' \
        r'-----BEGIN PGP PRIVATE KEY BLOCK-----.*' \
        r'-----END PGP PRIVATE KEY BLOCK-----.*$'
        SEC_KEY_PKTS = r'(?s)^.*' \
        r'Secret key packet.*' \
        r'keyid: 0x0451409669ffde3c.*' \
        r'Secret subkey packet.*' \
        r'keyid: 0xdd23ceb7febeff17.*$'
        KEY_OVERWRITE = r'(?s)^.*' \
        r'File \'.*alice-key.pub.asc\' already exists.*' \
        r'Would you like to overwrite it\? \(y/N\).*' \
        r'Please enter the new filename:.*$'

        clear_keyrings()
        # Import Alice's public key
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_SUB_PUB)])
        self.assertEqual(ret, 0)
        # Export all keys (no search pattern)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, PUB_KEY)
        # Attempt to export wrong key
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'boris'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Key\(s\) matching \'boris\' not found\.$')
        # Export it to the stdout
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, PUB_KEY)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice', '--output', '-'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, PUB_KEY)
        # Export key via --userid parameter
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', '--userid', 'alice'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, PUB_KEY)
        # Export with empty --userid parameter
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', '--userid'])
        self.assertNotEqual(ret, 0)
        # Export it to the file
        kpub, ksec, kren = reg_workfiles('alice-key', '.pub.asc', '.sec.asc', '.pub.ren-asc')
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice', '--output', kpub])
        self.assertEqual(ret, 0)
        self.assertRegex(file_text(kpub), PUB_KEY)
        # Try to export again to the same file without additional parameters
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice', '--output', kpub, '--notty'], '\n\n')
        self.assertNotEqual(ret, 0)
        self.assertRegex(out, KEY_OVERWRITE)
        self.assertRegex(err, r'(?s)^.*Operation failed: file \'.*alice-key.pub.asc\' already exists.*$')
        # Try to export with --force parameter
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice', '--output', kpub, '--force', '--notty'], '\n\n')
        self.assertNotEqual(ret, 0)
        self.assertRegex(out, KEY_OVERWRITE)
        self.assertRegex(err, r'(?s)^.*Operation failed: file \'.*alice-key.pub.asc\' already exists.*$')
        # Export with --overwrite parameter
        with open(kpub, 'w+') as f:
            f.truncate(10)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice', '--output', kpub, '--overwrite'])
        self.assertEqual(ret, 0)
        # Re-import it, making sure file was correctly overwritten
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', kpub])
        self.assertEqual(ret, 0)
        # Enter 'y' in overwrite prompt
        with open(kpub, 'w+') as f:
            f.truncate(10)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice', '--output', kpub, '--notty'], 'y\n')
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', kpub])
        self.assertEqual(ret, 0)
        # Enter new filename in overwrite prompt
        with open(kpub, 'w+') as f:
            f.truncate(10)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice', '--output', kpub, '--notty'], 'n\n' + kren + '\n')
        self.assertEqual(ret, 0)
        self.assertEqual(os.path.getsize(kpub), 10)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', kren])
        self.assertEqual(ret, 0)
        # Attempt to export secret key
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', '--secret', 'alice'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Key\(s\) matching \'alice\' not found\.$')
        # Import Alice's secret key and subkey
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_SUB_SEC)])
        self.assertEqual(ret, 0)
        # Make sure secret key is not exported when public is requested
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', 'alice', '--output', ksec])
        self.assertEqual(ret, 0)
        self.assertRegex(file_text(ksec), PUB_KEY)
        ret, out, _ = run_proc(RNP, ['--list-packets', ksec])
        self.assertEqual(ret, 0)
        self.assertRegex(out, PUB_KEY_PKTS)
        # Make sure secret key is correctly exported
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export-key', '--secret', 'alice', '--output', ksec, '--overwrite'])
        self.assertEqual(ret, 0)
        self.assertRegex(file_text(ksec), SEC_KEY)
        ret, out, _ = run_proc(RNP, ['--list-packets', ksec])
        self.assertEqual(ret, 0)
        self.assertRegex(out, SEC_KEY_PKTS)
        clear_keyrings()

    def test_userid_escape(self):
        clear_keyrings()
        tracker_beginning = 'tracker'
        tracker_end = '@rnp'
        tracker_1 = tracker_beginning + ''.join(map(chr, range(1,0x10))) + tracker_end
        tracker_2 = tracker_beginning + ''.join(map(chr, range(0x10,0x20))) + tracker_end
        #Run key generation
        rnp_genkey_rsa(tracker_1, 1024)
        rnp_genkey_rsa(tracker_2, 1024)
        #Read with rnpkeys
        ret, out_rnp, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0, 'rnpkeys : failed to read keystore')
        #Read with GPG
        ret, out_gpg, _ = run_proc(GPG, ['--homedir', path_for_gpg(RNPDIR), '--list-keys'])
        self.assertEqual(ret, 0, 'gpg : failed to read keystore')
        tracker_rnp = re.findall(r'' + tracker_beginning + '.*' + tracker_end + '', out_rnp)
        tracker_gpg = re.findall(r'' + tracker_beginning + '.*' + tracker_end + '', out_gpg)
        self.assertEqual(len(tracker_rnp), 2, 'failed to find expected rnp userids')
        self.assertEqual(len(tracker_gpg), 2, 'failed to find expected gpg userids')
        self.assertEqual(tracker_rnp, tracker_gpg, 'userids from rnpkeys and gpg don\'t match')
        clear_keyrings()

    def test_key_revoke(self):
        clear_keyrings()
        # Import Alice's public key and be unable to revoke
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_PUB)])
        self.assertEqual(ret, 0, ALICE_IMPORT_FAIL)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke-key', 'alice'])
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*Revoker secret key not found.*Failed to revoke a key.*')
        # Import Alice's secret key and subkey
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_SUB_SEC)])
        self.assertEqual(ret, 0)
        # Attempt to revoke without specifying a key
        pipe = pswd_pipe(PASSWORD)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*You need to specify key or subkey to revoke.*')
        # Attempt to revoke unknown key
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', 'basil'])
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*Key matching \'basil\' not found.*')
        # Attempt to revoke with too broad search
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_validity/basil-sec.asc')])
        self.assertEqual(ret, 0)
        pipe = pswd_pipe(PASSWORD)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', 'rnp', '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertRegex(err, r'(?s)^.*Ambiguous input: too many keys found for \'rnp\'.*')
        # Revoke a primary key
        pipe = pswd_pipe(PASSWORD)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', '0451409669FFDE3C', '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*pub.*0451409669ffde3c.*\[REVOKED\].*73edcc9119afc8e2dbbdcde50451409669ffde3c.*')
        # Try again without the '--force' parameter
        pipe = pswd_pipe(PASSWORD)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', '0451409669FFDE3C', '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*Error: key \'0451409669FFDE3C\' is revoked already. Use --force to generate another revocation signature.*')
        # Try again with --force parameter
        pipe = pswd_pipe(PASSWORD)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', '0451409669FFDE3C', '--pass-fd', str(pipe), "--force", "--rev-type", "3", "--rev-reason", "Custom"])
        os.close(pipe)
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*pub.*0451409669ffde3c.*\[REVOKED\].*73edcc9119afc8e2dbbdcde50451409669ffde3c.*')
        # Revoke a subkey
        pipe = pswd_pipe(PASSWORD)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', 'DD23CEB7FEBEFF17', '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*sub.*dd23ceb7febeff17.*\[REVOKED\].*a4bbb77370217bca2307ad0ddd23ceb7febeff17.*')
        # Try again without the '--force' parameter
        pipe = pswd_pipe(PASSWORD)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', 'DD23CEB7FEBEFF17', '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertNotEqual(ret, 0)
        self.assertEqual(len(out), 0)
        self.assertRegex(err, r'(?s)^.*Error: key \'DD23CEB7FEBEFF17\' is revoked already. Use --force to generate another revocation signature.*', err)
        # Try again with --force parameter
        pipe = pswd_pipe(PASSWORD)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--revoke', 'DD23CEB7FEBEFF17', '--pass-fd', str(pipe), "--force", "--rev-type", "2", "--rev-reason", "Other"])
        os.close(pipe)
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*sub.*dd23ceb7febeff17.*\[REVOKED\].*a4bbb77370217bca2307ad0ddd23ceb7febeff17.*')

    def _test_userid_genkey(self, userid_beginning, weird_part, userid_end, weird_part2=''):
        clear_keyrings()
        USERS = [userid_beginning + weird_part + userid_end]
        if weird_part2:
            USERS.append(userid_beginning + weird_part2 + userid_end)
        # Run key generation
        for userid in USERS:
            rnp_genkey_rsa(userid, 1024)
        # Read with GPG
        ret, out, _ = run_proc(GPG, ['--homedir', path_for_gpg(RNPDIR), '--list-keys', '--charset', CONSOLE_ENCODING])
        self.assertEqual(ret, 0, 'gpg : failed to read keystore')
        tracker_escaped = re.findall(r'' + userid_beginning + '.*' + userid_end + '', out)
        tracker_gpg = list(map(decode_string_escape, tracker_escaped))
        self.assertEqual(tracker_gpg, USERS, 'gpg : failed to find expected userids from keystore')
        # Read with rnpkeys
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0, 'rnpkeys : failed to read keystore')
        tracker_escaped = re.findall(r'' + userid_beginning + '.*' + userid_end + '', out)
        tracker_rnp = list(map(decode_string_escape, tracker_escaped))
        self.assertEqual(tracker_rnp, USERS, 'rnpkeys : failed to find expected userids from keystore')
        clear_keyrings()

    def test_userid_unicode_genkeys(self):
        self._test_userid_genkey('track', WEIRD_USERID_UNICODE_1, 'end', WEIRD_USERID_UNICODE_2)

    def test_userid_special_chars_genkeys(self):
        self._test_userid_genkey('track', WEIRD_USERID_SPECIAL_CHARS, 'end')
        self._test_userid_genkey('track', WEIRD_USERID_SPACE, 'end')
        self._test_userid_genkey('track', WEIRD_USERID_QUOTE, 'end')
        self._test_userid_genkey('track', WEIRD_USERID_SPACE_AND_QUOTE, 'end')

    def test_userid_too_long_genkeys(self):
        clear_keyrings()
        userid = WEIRD_USERID_TOO_LONG
        # Open pipe for password
        pipe = pswd_pipe(PASSWORD)
        # Run key generation
        ret, _, _ = run_proc(RNPK, ['--gen-key', '--userid', userid,
                                    '--homedir', RNPDIR, '--pass-fd', str(pipe)])
        os.close(pipe)
        self.assertNotEqual(ret, 0, 'should have failed on too long id')

    def test_key_remove(self):
        if not RNP_CAST5:
            self.skipTest("No CAST5 support")
        MSG_KEYS_NOT_FOUND = r'Key\(s\) not found\.'
        clear_keyrings()
        # Import public keyring
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(PUBRING_1)])
        self.assertEqual(ret, 0)
        # Remove without parameters
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key'])
        self.assertNotEqual(ret, 0)
        # Remove all imported public keys with subkeys
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', '7bc6709b15c23a4a', '2fcadf05ffa501bb'])
        self.assertEqual(ret, 0)
        # Check that keyring is empty
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(out, MSG_KEYS_NOT_FOUND, 'Invalid no-keys output')
        # Import secret keyring
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('keyrings/1/secring.gpg')])
        self.assertEqual(ret, 0, 'Secret keyring import failed')
        # Remove all secret keys with subkeys
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', '7bc6709b15c23a4a', '2fcadf05ffa501bb', '--force'])
        self.assertEqual(ret, 0, 'Failed to remove 2 secret keys')
        # Check that keyring is empty
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(out, MSG_KEYS_NOT_FOUND, 'Failed to remove secret keys')
        # Import public keyring
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(PUBRING_1)])
        self.assertEqual(ret, 0, 'Public keyring import failed')
        # Remove all subkeys
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key',
                                        '326ef111425d14a5', '54505a936a4a970e', '8a05b89fad5aded1', '1d7e8a5393c997a8', '1ed63ee56fadc34d'])
        self.assertEqual(ret, 0, 'Failed to remove 5 keys')
        # Check that subkeys are removed
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'2 keys found', 'Failed to remove subkeys')
        self.assertFalse(re.search('326ef111425d14a5|54505a936a4a970e|8a05b89fad5aded1|1d7e8a5393c997a8|1ed63ee56fadc34d', out))
        # Remove remaining public keys
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', '7bc6709b15c23a4a', '2fcadf05ffa501bb'])
        self.assertEqual(ret, 0, 'Failed to remove public keys')
        # Try to remove again
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', '7bc6709b15c23a4a'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'Key matching \'7bc6709b15c23a4a\' not found\.', 'Unexpected result')
        # Check that keyring is empty
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertRegex(out, MSG_KEYS_NOT_FOUND, 'Failed to list empty keyring')
        # Import public keyring
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(PUBRING_1)])
        self.assertEqual(ret, 0, 'Public keyring import failed')
        # Try to remove by uid substring, should match multiple keys and refuse to remove
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', 'uid0'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'Ambiguous input: too many keys found for \'uid0\'\.', 'Unexpected result')
        # Remove keys by uids
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', 'key0-uid0', 'key1-uid1'])
        self.assertEqual(ret, 0, 'Failed to remove keys')
        # Check that keyring is empty
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(out, MSG_KEYS_NOT_FOUND, 'Failed to remove keys')

    def test_additional_subkeys_default(self):
        '''
        Generate default key (primary + sub) then add more subkeys.
        '''
        # Open pipe for password
        pipe = pswd_pipe(PASSWORD)
        # Run key generation
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--pass-fd', str(pipe),
                                    '--userid', 'primary_for_many_subs@rnp', '--generate-key'])
        os.close(pipe)
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Edit generated key, generate & add one more subkey with default parameters
        pipe = pswd_pipe(PASSWORD)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--pass-fd', str(pipe),
                                    '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'])
        self.assertEqual(ret, 0, 'Failed to add new subkey')
        # list keys, check result
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0, KEY_LIST_FAILED)
        self.assertRegex(out, RE_MULTIPLE_SUBKEY_3, KEY_LIST_WRONG)
        clear_keyrings()

    def test_expert_mode_no_endless_loop(self):
        TOO_MANY=r'(?s)Too many attempts. Aborting.'
        UID='noendlessloop@rnp'
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', UID, '--expert', '--generate-key'],
                                      '\n\n\n\n\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, TOO_MANY)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', UID, '--expert', '--generate-key'],
                                      '1\n1\n1\n1\n1\n1\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, TOO_MANY)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', UID, '--expert', '--generate-key'],
                                      '16\n1\n1\n1\n1\n1\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, TOO_MANY)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', UID, '--expert', '--generate-key'],
                                      '17\n1\n1\n1\n1\n1\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, TOO_MANY)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', UID, '--expert', '--generate-key'],
                                      '19\n\n\n\n\n\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, TOO_MANY)

        clear_keyrings()

    def test_additional_subkeys_invalid_parameters(self):
        # Run primary key generation
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                    '--userid', 'primary_for_many_subs@rnp', '--generate-key'])
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Attempt to generate subkey for non-existing key
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                      '--edit-key', '--add-subkey', 'unknown'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'Secret keys matching \'unknown\' not found.')
        # Attempt to generate subkey using the invalid password
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--password', 'wrong',
                                      '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)Failed to unlock primary key.*Subkey generation failed')
        # Attempt to generate subkey using the invalid password, asked via tty
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--edit-key',
                                      '--add-subkey', 'primary_for_many_subs@rnp'], 'password2\n')
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)Failed to unlock primary key.*Subkey generation failed')
        # Attempt to generate ECDH subkey with invalid curve
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD, '--edit-key', '--add-subkey',
                                      'primary_for_many_subs@rnp', '--expert'],
                                      '\n\n0\n101\n18\n-10\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, r'(?s)Too many attempts. Aborting.')
        self.assertRegex(err, r'(?s)Subkey generation setup failed')
        # Attempt to generate ECDSA subkey with invalid curve
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD, '--edit-key', '--add-subkey',
                                      'primary_for_many_subs@rnp', '--expert'],
                                      '19\n-10\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n0\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, r'(?s)Too many attempts. Aborting.')
        self.assertRegex(err, r'(?s)Subkey generation setup failed')
        # Pass invalid numbits
        ret, _, err = run_proc(RNPK, ['--numbits', 'wrong', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', 'wrong', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*wrong bits value: wrong.*')
        # Too small
        ret, _, err = run_proc(RNPK, ['--numbits', '768', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', '768', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*wrong bits value: 768.*')
        # ElGamal too large and wrong numbits
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--password', 'wrong', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp',
                                        '--expert'], '16\n2048zzz\n99999999999999999999999999\n2048\n')
        self.assertRegex(err, r'(?s)Unexpected end of line.*Number out of range.*')
        self.assertEqual(ret, 1)
        # Wrong hash algorithm
        ret, _, err = run_proc(RNPK, ['--hash', 'BAD_HASH', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', 'bad_hash', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Unsupported hash algorithm: BAD_HASH.*')
        # Wrong S2K iterations
        ret, _, err = run_proc(RNPK, ['--s2k-iterations', 'WRONG_ITER', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', 'wrong_iter', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Wrong iterations value: WRONG_ITER.*')
        # Wrong S2K msec
        ret, _, err = run_proc(RNPK, ['--s2k-msec', 'WRONG_MSEC', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', 'wrong_msec', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Invalid s2k msec value: WRONG_MSEC.*')
        # Wrong cipher
        ret, _, err = run_proc(RNPK, ['--cipher', 'WRONG_AES', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--userid', 'wrong_aes', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Unsupported encryption algorithm: WRONG_AES.*Failed to process argument --cipher.*')
        # Ambiguous primary key
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                    '--userid', 'primary_for_many_subs2@rnp', '--generate-key'])
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--password', PASSWORD,
                                      '--edit-key', '--add-subkey', 'primary_for_many'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'Ambiguous input: too many keys found for \'primary_for_many\'')

        clear_keyrings()

    def test_additional_subkeys_expert_mode(self):
        # Run primary key generation
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=',
                                    '--userid', 'primary_for_many_subs@rnp', '--generate-key'])
        # RSA subkey
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp',
                                      '--expert'], '\n\n0\n101\n1\n1023\n4097\n3072\n')
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # ElGamal subkey
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp',
                                      '--expert'], '\n\n0\n101\n16\n1023\n4097\n1025\n')
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # DSA subkey
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp',
                                      '--expert'], '\n\n0\n101\n17\n1023\n3073\n1025\n')
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # ECDH subkey
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp',
                                      '--expert'], '\n\n0\n101\n18\n0\n8\n1\n')
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # ECDSA subkey
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp',
                                      '--expert'], '\n\n0\n101\n19\n0\n8\n1\n')
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # EDDSA subkey
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp',
                                      '--expert'], '\n\n0\n101\n22\n')
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # list keys, check result
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0, KEY_LIST_FAILED)
        self.assertRegex(out, RE_MULTIPLE_SUBKEY_8, KEY_LIST_WRONG)

        clear_keyrings()

    def test_additional_subkeys_reuse_password(self):
        pipe = pswd_pipe('primarypassword')
        # Primary key with password
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--pass-fd', str(pipe),
                                    '--userid', 'primary_for_many_subs@rnp', '--generate-key'])
        os.close(pipe)
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Provide password to add subkey, reuse password for subkey, say "yes"
        stdinstr = 'primarypassword\ny\n'
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'],
                             stdinstr)
        self.assertEqual(ret, 0, 'Failed to add new subkey')
        self.assertRegex(out, r'Would you like to use the same password to protect subkey')
        # Do not reuse same password for subkey, say "no"
        stdinstr = 'primarypassword\nN\nsubkeypassword\nsubkeypassword\n'
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--edit-key', '--add-subkey', 'primary_for_many_subs@rnp'],
                             stdinstr)
        self.assertEqual(ret, 0)
        # Primary key with empty password
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=',
                                    '--userid', 'primary_with_empty_password@rnp', '--generate-key'])
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        # Set empty password for generated subkey
        stdinstr = '\n\ny\n'
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--edit-key', '--add-subkey', 'primary_with_empty_password@rnp'],
                             stdinstr)
        self.assertEqual(ret, 0)
        # Set password for generated subkey
        stdinstr = 'subkeypassword\nsubkeypassword\n'
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--edit-key', '--add-subkey', 'primary_with_empty_password@rnp'],
                             stdinstr)
        self.assertEqual(ret, 0)
        clear_keyrings()

    def test_edit_key_single_option(self):
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_25519_NOTWEAK_SEC)])
        self.assertEqual(ret, 0)
        # Try to pass multiple  --edit-key sub-options at once
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--check-cv25519-bits', '--fix-cv25519-bits',
                                      '--add-subkey', '--set-expire', '0', '3176fc1486aa2528'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Only one key edit option can be executed at a time..*$')
        clear_keyrings()

    def test_set_expire(self):
        kpath = os.path.join(RNPDIR, PUBRING)
        # Primary key with empty password
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=',
                                    '--userid', 'primary_with_empty_password@rnp', '--generate-key'])
        self.assertEqual(ret, 0, KEY_GEN_FAILED)

        # Wrong expiration argument
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--set-expire', '-1', 'primary_with_empty_password@rnp'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'Failed to set key expiration.')

        ret, out, _ = run_proc(RNP, ['--list-packets', kpath])
        self.assertEqual(ret, 0)
        matches = re.findall(r'(key expiration time: 63072000 seconds \(730 days\))', out)
        self.assertEqual(len(matches), 2)

        # Non-existing key argument
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--set-expire', '0', 'wrongkey'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'Secret keys matching \'wrongkey\' not found.')

        # Remove expiration date
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--set-expire', '0', 'primary_with_empty_password@rnp'])
        self.assertEqual(ret, 0)
        self.assertNotRegex(out, r'(?s)^.*\[EXPIRES .*', 'Failed to remove expiration!')

        ret, out, _ = run_proc(RNP, ['--list-packets', kpath])
        self.assertEqual(ret, 0)
        matches = re.findall(r'(key expiration time: 63072000 seconds \(730 days\))', out)
        self.assertEqual(len(matches), 1)

        # Expires in 60 seconds
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--set-expire', '60', 'primary_with_empty_password@rnp'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*\[EXPIRES .*')

        ret, out, _ = run_proc(RNP, ['--list-packets', kpath])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*key expiration time: 60 seconds \(0 days\).*')

        # Expires in 10 hours
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--set-expire', '10h', 'primary_with_empty_password@rnp'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*\[EXPIRES .*')
        ret, out, _ = run_proc(RNP, ['--list-packets', kpath])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*key expiration time: 36000 seconds \(0 days\).*')

        # Expires in 10 months
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--set-expire', '10m', 'primary_with_empty_password@rnp'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*\[EXPIRES .*')
        ret, out, _ = run_proc(RNP, ['--list-packets', kpath])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*key expiration time: 26784000 seconds \(310 days\).*')

        # Expires in 10 years
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--set-expire', '10y', 'primary_with_empty_password@rnp'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*\[EXPIRES .*')
        ret, out, _ = run_proc(RNP, ['--list-packets', kpath])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*key expiration time: 315360000 seconds \(3650 days\).*')

        # Additional primary for ambiguous key uid
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password=',
                                    '--userid', 'primary2@rnp', '--generate-key'])
        self.assertEqual(ret, 0, KEY_GEN_FAILED)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--set-expire', '0', 'primary'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'Ambiguous input: too many keys found for \'primary\'')

        clear_keyrings()

class Misc(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        rnp_genkey_rsa(KEY_ENCRYPT)
        rnp_genkey_rsa(KEY_SIGN_GPG)
        gpg_import_pubring()
        gpg_import_secring()

    @classmethod
    def tearDownClass(cls):
        clear_keyrings()

    def setUp(self):
        os.mkdir(RNPDIR2, 0o700)
        self.home = os.environ['HOME']

    def tearDown(self):
        os.environ['HOME'] = self.home
        clear_workfiles()
        shutil.rmtree(RNPDIR2, ignore_errors=True)

    def test_encryption_unicode(self):
        if sys.version_info >= (3,):
            filename = UNICODE_SEQUENCE_1
        else:
            filename = UNICODE_SEQUENCE_1.encode(CONSOLE_ENCODING)

        src, dst, dec = reg_workfiles(filename, '.txt', '.rnp', '.dec')
        # Generate random file of required size
        random_text(src, 128000)

        rnp_encrypt_file_ex(src, dst, [KEY_ENCRYPT])
        rnp_decrypt_file(dst, dec)
        compare_files(src, dec, RNP_DATA_DIFFERS)

        remove_files(src, dst, dec)

    def test_encryption_no_mdc(self):
        src, dst, dec = reg_workfiles('cleartext', '.txt', '.gpg', '.rnp')
        # Generate random file of required size
        random_text(src, 64000)
        # Encrypt cleartext file with GPG
        params = ['--homedir', GPGHOME, '-c', '-z', '0', '--disable-mdc', '--s2k-count',
                  '65536', '--batch', '--passphrase', PASSWORD, '--output',
                  path_for_gpg(dst), path_for_gpg(src)]
        ret, _, _ = run_proc(GPG, params)
        self.assertEqual(ret, 0, 'gpg symmetric encryption failed')
        # Decrypt encrypted file with RNP
        rnp_decrypt_file(dst, dec)
        compare_files(src, dec, RNP_DATA_DIFFERS)

    def test_encryption_s2k(self):
        src, dst, dec = reg_workfiles('cleartext', '.txt', '.gpg', '.rnp')
        random_text(src, 1000)

        ciphers = rnp_supported_ciphers(False)
        hashes = ['SHA1', 'RIPEMD160', 'SHA256', 'SHA384', 'SHA512', 'SHA224']
        s2kmodes = [0, 1, 3]

        if not RNP_RIPEMD160:
            hashes.remove('RIPEMD160')

        def rnp_encryption_s2k_gpg(cipher, hash_alg, s2k=None, iterations=None):
            params = ['--homedir', GPGHOME, '-c', '--s2k-cipher-algo', cipher,
                      '--s2k-digest-algo', hash_alg, '--batch', '--passphrase', PASSWORD,
                      '--output', dst, src]

            if s2k is not None:
                params.insert(7, '--s2k-mode')
                params.insert(8, str(s2k))

                if iterations is not None:
                    params.insert(9, '--s2k-count')
                    params.insert(10, str(iterations))

            if GPG_NO_OLD:
                params.insert(3, '--allow-old-cipher-algos')

            ret, _, _ = run_proc(GPG, params)
            self.assertEqual(ret, 0, 'gpg symmetric encryption failed')
            rnp_decrypt_file(dst, dec)
            compare_files(src, dec, RNP_DATA_DIFFERS)
            remove_files(dst, dec)

        for i in range(0, 20):
            rnp_encryption_s2k_gpg(ciphers[i % len(ciphers)], hashes[
                                i % len(hashes)], s2kmodes[i % len(s2kmodes)])

    def test_armor(self):
        src_beg, dst_beg, dst_mid, dst_fin = reg_workfiles('beg', '.src', '.dst',
                                                           '.mid.dst', '.fin.dst')
        armor_types = [('msg', 'MESSAGE'), ('pubkey', 'PUBLIC KEY BLOCK'),
                       ('seckey', 'PRIVATE KEY BLOCK'), ('sign', 'SIGNATURE')]

        random_text(src_beg, 1000)
        # Wrong armor type
        ret, _, err = run_proc(RNP, ['--enarmor=wrong', src_beg, '--output', dst_beg])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Wrong enarmor argument: wrong.*$')

        # Default armor type
        ret, _, _ = run_proc(RNP, ['--enarmor', src_beg, '--output', dst_beg])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Wrong enarmor argument: wrong.*$')
        txt = file_text(dst_beg).strip('\r\n')
        self.assertTrue(txt.startswith('-----BEGIN PGP MESSAGE-----'), 'wrong armor header')
        self.assertTrue(txt.endswith('-----END PGP MESSAGE-----'), 'wrong armor trailer')
        remove_files(dst_beg)

        for data_type, header in armor_types:
            prefix = '-----BEGIN PGP ' + header + '-----'
            suffix = '-----END PGP ' + header + '-----'

            ret, _, _ = run_proc(RNP, ['--enarmor=' + data_type, src_beg, '--output', dst_beg])
            self.assertEqual(ret, 0)
            txt = file_text(dst_beg).strip('\r\n')

            self.assertTrue(txt.startswith(prefix), 'wrong armor header')
            self.assertTrue(txt.endswith(suffix), 'wrong armor trailer')

            ret, _, _ = run_proc(RNP, ['--dearmor', dst_beg, '--output', dst_mid])
            self.assertEqual(ret, 0)
            ret, _, _ = run_proc(RNP, ['--enarmor=' + data_type, dst_mid, '--output', dst_fin])
            self.assertEqual(ret, 0)

            compare_files(dst_beg, dst_fin, "RNP armor/dearmor test failed")
            compare_files(src_beg, dst_mid, "RNP armor/dearmor test failed")
            remove_files(dst_beg, dst_mid, dst_fin)

        # 3-byte last chunk with missing crc
        msg = '-----BEGIN PGP MESSAGE-----\n\nMTIzNDU2Nzg5\n-----END PGP MESSAGE-----\n'
        ret, out, err = run_proc(RNP, ['--dearmor'], msg)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*123456789.*')
        self.assertRegex(err, r'(?s)^.*Warning: missing or malformed CRC line.*')
        # No invalid CRC message
        R_CRC = r'(?s)^.*Warning: CRC mismatch.*$'
        dec = 'decoded.pgp'
        ret, _, err = run_proc(RNP, ['--dearmor', data_path('test_stream_key_load/ecc-25519-pub.asc'), '--output', dec])
        remove_files(dec)
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, R_CRC)
        # Invalid CRC message
        ret, _, err = run_proc(RNP, ['--dearmor', data_path('test_stream_armor/ecc-25519-pub-bad-crc.asc'), '--output', dec])
        remove_files(dec)
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_CRC)
        # 1-byte message
        ret, out, _ = run_proc(RNP, ['--enarmor'], '1')
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*BEGIN PGP MESSAGE.*MQ==.*=LrpI.*')
        ret, out, _ = run_proc(RNP, ['--dearmor'], out)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*1.*')
        # Charset and other headers
        ret, _, err = run_proc(RNP, ['--dearmor', data_path('test_stream_armor/misc_headers.asc'), '--output', dec], out)
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*unknown header \'Unknown: Unknown\'.*')
        remove_files(dec)

    def test_rnpkeys_lists(self):
        KEYRING_2 = data_path(KEYRING_DIR_2)
        KEYRING_3 = data_path(KEYRING_DIR_3)
        KEYRING_5 = data_path('keyrings/5')
        path = data_path('test_cli_rnpkeys') + '/'

        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_2, '--list-keys'])
        compare_file(path + 'keyring_2_list_keys', out, 'keyring 2 key listing failed')
        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_2, '-l', '--with-sigs'])
        compare_file(path + 'keyring_2_list_sigs', out, 'keyring 2 sig listing failed')

        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_3, '--list-keys'])
        compare_file_any(allow_y2k38_on_32bit(path + 'keyring_3_list_keys'), out, 'keyring 3 key listing failed')
        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_3, '-l', '--with-sigs'])
        compare_file_any(allow_y2k38_on_32bit(path + 'keyring_3_list_sigs'), out, 'keyring 3 sig listing failed')

        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_5, '--list-keys'])
        compare_file(path + 'keyring_5_list_keys', out, 'keyring 5 key listing failed')
        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_5, '-l', '--with-sigs'])
        compare_file(path + 'keyring_5_list_sigs', out, 'keyring 5 sig listing failed')

        # Below are disabled until we have some kind of sorting which doesn't depend on
        # readdir order
        #_, out, _ = run_proc(RNPK, ['--homedir', data_path(SECRING_G10),
        #                            '-l', '--secret'])
        #compare_file(path + 'test_stream_key_load_keys_sec', out,
        #             'g10 sec keyring key listing failed')
        #_, out, _ = run_proc(RNPK, ['--homedir', data_path(SECRING_G10),
        #                            '-l', '--secret', '--with-sigs'])
        #compare_file(path + 'test_stream_key_load_sigs_sec', out,
        #             'g10 sec keyring sig listing failed')

    def test_rnpkeys_lists_bp(self):
        path = data_path('test_cli_rnpkeys') + '/'
        _, out, _ = run_proc(RNPK, ['--homedir', data_path(SECRING_G10), '--list-keys'])
        if RNP_BRAINPOOL:
            self.assertEqual(file_text(path + 'test_stream_key_load_keys'), out, 'g10 keyring key listing failed')
        else:
            self.assertEqual(file_text(path + 'test_stream_key_load_keys_no_bp'), out, 'g10 keyring key listing failed')
        _, out, _ = run_proc(RNPK, ['--homedir', data_path(SECRING_G10), '-l', '--with-sigs'])
        if RNP_BRAINPOOL:
            self.assertEqual(file_text(path + 'test_stream_key_load_sigs'), out, 'g10 keyring sig listing failed')
        else:
            self.assertEqual(file_text(path + 'test_stream_key_load_sigs_no_bp'), out, 'g10 keyring sig listing failed')

    def test_rnpkeys_lists_cast5(self):
        if not RNP_CAST5:
            self.skipTest("No CAST5 support")

        KEYRING_1 = data_path(KEYRING_DIR_1)
        path = data_path('test_cli_rnpkeys') + '/'

        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_1, '--list-keys'])
        compare_file_any(allow_y2k38_on_32bit(path + 'keyring_1_list_keys'), out, 'keyring 1 key listing failed')
        _, out, _ = run_proc(RNPK, ['--home', KEYRING_1, '-l', '--with-sigs'])
        compare_file_any(allow_y2k38_on_32bit(path + 'keyring_1_list_sigs'), out, 'keyring 1 sig listing failed')
        _, out, _ = run_proc(RNPK, ['--home', KEYRING_1, '--list-keys', '--secret'])
        compare_file_any(allow_y2k38_on_32bit(path + 'keyring_1_list_keys_sec'), out, 'keyring 1 sec key listing failed')
        _, out, _ = run_proc(RNPK, ['--home', KEYRING_1, '--list-keys',
                                    '--secret', '--with-sigs'])
        compare_file_any(allow_y2k38_on_32bit(path + 'keyring_1_list_sigs_sec'), out, 'keyring 1 sec sig listing failed')

        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_1, '-l', '2fcadf05ffa501bb'])
        compare_file_any(allow_y2k38_on_32bit(path + 'getkey_2fcadf05ffa501bb'), out, 'list key 2fcadf05ffa501bb failed')
        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_1, '-l',
                                    '--with-sigs', '2fcadf05ffa501bb'])
        compare_file_any(allow_y2k38_on_32bit(path + 'getkey_2fcadf05ffa501bb_sig'), out, 'list sig 2fcadf05ffa501bb failed')
        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_1, '-l',
                                    '--secret', '2fcadf05ffa501bb'])
        compare_file_any(allow_y2k38_on_32bit(path + 'getkey_2fcadf05ffa501bb_sec'), out, 'list sec 2fcadf05ffa501bb failed')

        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_1, '-l', '00000000'])
        compare_file(path + 'getkey_00000000', out, 'list key 00000000 failed')
        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_1, '-l', 'zzzzzzzz'])
        compare_file(path + 'getkey_zzzzzzzz', out, 'list key zzzzzzzz failed')

        _, out, _ = run_proc(RNPK, ['--homedir', KEYRING_1, '-l', '--userid', '2fcadf05ffa501bb'])
        compare_file_any(allow_y2k38_on_32bit(path + 'getkey_2fcadf05ffa501bb'), out, 'list key 2fcadf05ffa501bb failed')

    def test_rnpkeys_list_invalid_keys(self):
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('test_forged_keys/eddsa-2012-md5-pub.pgp')])
        self.assertEqual(ret, 0)
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR2, '--list-keys', '--with-sigs'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)2 keys found.*8801eafbd906bd21.*\[INVALID\].*expired-md5-key-sig.*\[INVALID\].*sig.*\[unknown\] \[invalid\]')
        self.assertRegex(err, r'(?s)Insecure hash algorithm 1, marking signature as invalid')

    def test_rnpkeys_g10_list_order(self):
        ret, out, _ = run_proc(RNPK, ['--homedir', data_path(SECRING_G10), '--list-keys'])
        self.assertEqual(ret, 0)
        if RNP_BRAINPOOL:
            self.assertEqual(file_text(data_path('test_cli_rnpkeys/g10_list_keys')), out, 'g10 key listing failed')
        else:
            self.assertEqual(file_text(data_path('test_cli_rnpkeys/g10_list_keys_no_bp')), out, 'g10 key listing failed')
        ret, out, _ = run_proc(RNPK, ['--homedir', data_path(SECRING_G10), '--secret', '--list-keys'])
        self.assertEqual(ret, 0)
        if RNP_BRAINPOOL:
            self.assertEqual(file_text(data_path('test_cli_rnpkeys/g10_list_keys_sec')), out, 'g10 secret key listing failed')
        else:
            self.assertEqual(file_text(data_path('test_cli_rnpkeys/g10_list_keys_sec_no_bp')), out, 'g10 secret key listing failed')

    def test_rnpkeys_list_from_keyfile(self):
        KEYRING_2 = data_path(KEYRING_DIR_2)
        ret, out, err = run_proc(RNPK, ['--homedir', KEYRING_2, '--list-keys', '--keyfile', data_path(KEY_ALICE_PUB)])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'1 key found.*')
        self.assertRegex(out, r'(?s)^.*73edcc9119afc8e2dbbdcde50451409669ffde3c.*Alice')
        self.assertNotRegex(out, r'(?s)^.*c80aa54aa5c6ac73a373687134abe4bd')
        ret, out, err = run_proc(RNPK, ['--homedir', KEYRING_2, '--list-keys', '--keyfile', 'wrongkeyfile'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*fatal: failed to load key\(s\) from the file')

    def test_rnpkeys_g10_def_key(self):
        RE_SIG = r'(?s)^.*' \
        r'Good signature made .*' \
        r'using (.*) key (.*)' \
        r'pub .*' \
        r'b54fdebbb673423a5d0aa54423674f21b2441527.*' \
        r'uid\s+(ecc-p256)\s*' \
        r'Signature\(s\) verified successfully.*$'

        src, dst = reg_workfiles('cleartext', '.txt', '.rnp')
        random_text(src, 1000)
        # Sign file with rnp using the default g10 key
        params = ['--homedir', data_path('test_cli_g10_defkey/g10'),
                  '--password', PASSWORD, '--output', dst, '-s', src]
        ret, _, err = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'rnp signing failed')
        # Verify signed file
        params = ['--homedir', data_path('test_cli_g10_defkey/g10'), '-v', dst]
        ret, _, err = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'verification failed')
        self.assertRegex(err, RE_SIG, 'wrong rnp g10 verification output')

    def test_large_packet(self):
        # Verifying large packet file with GnuPG
        kpath = path_for_gpg(data_path(PUBRING_1))
        dpath = path_for_gpg(data_path('test_large_packet/4g.bzip2.gpg'))
        ret, _, _ = run_proc(GPG, ['--homedir', GPGHOME, '--no-default-keyring', '--keyring', kpath, '--verify', dpath])
        self.assertEqual(ret, 0, 'large packet verification failed')

    def test_partial_length_signature(self):
        # Verifying partial length signature with GnuPG
        kpath = path_for_gpg(data_path(PUBRING_1))
        mpath = path_for_gpg(data_path('test_partial_length/message.txt.partial-signed'))
        ret, _, _ = run_proc(GPG, ['--homedir', GPGHOME, '--no-default-keyring', '--keyring', kpath, '--verify', mpath])
        self.assertNotEqual(ret, 0, 'partial length signature packet should result in failure but did not')

    def test_partial_length_public_key(self):
        # Reading keyring that has a public key packet with partial length using GnuPG
        kpath = data_path('test_partial_length/pubring.gpg.partial')
        ret, _, _ = run_proc(GPG, ['--homedir', GPGHOME, '--no-default-keyring', '--keyring', kpath, '--list-keys'])
        self.assertNotEqual(ret, 0, 'partial length public key packet should result in failure but did not')

    def test_partial_length_zero_last_chunk(self):
        # Verifying message in partial packets having 0-size last chunk with GnuPG
        kpath = path_for_gpg(data_path(PUBRING_1))
        mpath = path_for_gpg(data_path('test_partial_length/message.txt.partial-zero-last'))
        ret, _, _ = run_proc(GPG, ['--homedir', GPGHOME, '--no-default-keyring', '--keyring', kpath, '--verify', mpath])
        self.assertEqual(ret, 0, 'message in partial packets having 0-size last chunk verification failed')

    def test_partial_length_largest(self):
        # Verifying message having largest possible partial packet with GnuPG
        kpath = path_for_gpg(data_path(PUBRING_1))
        mpath = path_for_gpg(data_path('test_partial_length/message.txt.partial-1g'))
        ret, _, _ = run_proc(GPG, ['--homedir', GPGHOME, '--no-default-keyring', '--keyring', kpath, '--verify', mpath])
        self.assertEqual(ret, 0, 'message having largest possible partial packet verification failed')

    def test_rnp_single_export(self):
        # Import key with subkeys, then export it, test that it is exported once.
        # See issue #1153
        clear_keyrings()
        # Import Alice's secret key and subkey
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_SUB_SEC)])
        self.assertEqual(ret, 0, 'Alice secret key import failed')
        # Export key
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export', 'Alice'])
        self.assertEqual(ret, 0, 'key export failed')
        pubpath = os.path.join(RNPDIR, 'Alice-export-test.asc')
        with open(pubpath, 'w+') as f:
            f.write(out)
        # List exported key packets
        params = ['--list-packets', pubpath]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_single_export_subkeys/list_key_export_single.txt'), out,
                        'exported packets mismatch')

    def test_rnp_permissive_key_import(self):
        # Import keys while skipping bad packets, see #1160
        clear_keyrings()
        # Try to import  without --permissive option, should fail.
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_key_edge_cases/pubring-malf-cert.pgp')])
        self.assertNotEqual(ret, 0, 'Imported bad packets without --permissive option set!')
        # Import with --permissive
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', '--permissive',data_path('test_key_edge_cases/pubring-malf-cert.pgp')])
        self.assertEqual(ret, 0, 'Failed to import keys with --permissive option')

        # List imported keys and sigs
        params = ['--homedir', RNPDIR, '--list-keys', '--with-sigs']
        ret, out, _ = run_proc(RNPK, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_any(allow_y2k38_on_32bit(data_path('test_cli_rnpkeys/pubring-malf-cert-permissive-import.txt')),
            out, 'listing mismatch')

    def test_rnp_autocrypt_key_import(self):
        R_25519 = r'(?s)^.*pub.*255/EdDSA.*21fc68274aae3b5de39a4277cc786278981b0728.*$'
        R_256K1 = r'(?s)^.*pub.*3ea5bb6f9692c1a0.*7635401f90d3e533.*$'
        # Import misc configurations of base64-encoded autocrypt keys
        clear_keyrings()
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_stream_key_load/ecc-25519-pub.b64')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, R_25519)
        # No trailing EOL after the base64 data
        clear_keyrings()
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_stream_key_load/ecc-25519-pub-2.b64')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, R_25519)
        # Extra spaces/eols/tabs after the base64 data
        clear_keyrings()
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_stream_key_load/ecc-25519-pub-3.b64')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, R_25519)
        # Invalid symbols after the base64 data
        clear_keyrings()
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_stream_key_load/ecc-25519-pub-4.b64')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*wrong base64 padding: ==zz.*Failed to init/check dearmor.*failed to import key\(s\) from .*, stopping.*')
        # Binary data size is multiple of 3, single base64 line
        clear_keyrings()
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_stream_key_load/ecc-p256k1-pub.b64')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, R_256K1)
        # Binary data size is multiple of 3, multiple base64 lines
        clear_keyrings()
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_stream_key_load/ecc-p256k1-pub-2.b64')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, R_256K1)
        # Too long base64 trailer ('===')
        clear_keyrings()
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_stream_armor/long_b64_trailer.b64')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*wrong base64 padding length 3.*Failed to init/check dearmor.*$')
        # Extra data after the base64-encoded data
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import-keys', data_path('test_stream_armor/b64_trailer_extra_data.b64')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*warning: extra data after the base64 stream.*Failed to init/check dearmor.*warning: not all data was processed.*')
        self.assertRegex(out, R_25519)

    def test_rnp_list_packets(self):
        KEY_P256 = data_path('test_list_packets/ecc-p256-pub.asc')
        # List packets in human-readable format
        params = ['--list-packets', KEY_P256]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_list_packets/list_standard.txt'), out,
                        'standard listing mismatch')
        # List packets with mpi values
        params = ['--mpi', '--list-packets', KEY_P256]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'packet listing with mpi failed')
        compare_file_ex(data_path('test_list_packets/list_mpi.txt'), out, 'mpi listing mismatch')
        # List packets with grip/fingerprint values
        params = ['--list-packets', KEY_P256, '--grips']
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'packet listing with grips failed')
        compare_file_ex(data_path('test_list_packets/list_grips.txt'), out,
                        'grips listing mismatch')
        # List packets with raw packet contents
        params = ['--list-packets', KEY_P256, '--raw']
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'packet listing with raw packets failed')
        compare_file_ex(data_path('test_list_packets/list_raw.txt'), out, 'raw listing mismatch')
        # List packets with all options enabled
        params = ['--list-packets', KEY_P256, '--grips', '--raw', '--mpi']
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'packet listing with all options failed')
        compare_file_ex(data_path('test_list_packets/list_all.txt'), out, 'all listing mismatch')

        # List packets with JSON output
        params = ['--json', '--list-packets', KEY_P256]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'json packet listing failed')
        compare_file_ex(data_path('test_list_packets/list_json.txt'), out, 'json listing mismatch')
        # List packets with mpi values, JSON output
        params = ['--json', '--mpi', '--list-packets', KEY_P256]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'json mpi packet listing failed')
        compare_file_ex(data_path('test_list_packets/list_json_mpi.txt'), out,
                        'json mpi listing mismatch')
        # List packets with grip/fingerprint values, JSON output
        params = ['--json', '--grips', '--list-packets', KEY_P256]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'json grips packet listing failed')
        compare_file_ex(data_path('test_list_packets/list_json_grips.txt'), out,
                        'json grips listing mismatch')
        # List packets with raw packet values, JSON output
        params = ['--json', '--raw', '--list-packets', KEY_P256]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'json raw packet listing failed')
        compare_file_ex(data_path('test_list_packets/list_json_raw.txt'), out,
                        'json raw listing mismatch')
        # List packets with all values, JSON output
        params = ['--json', '--raw', '--list-packets', KEY_P256, '--mpi', '--grips']
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'json all listing failed')
        compare_file_ex(data_path('test_list_packets/list_json_all.txt'), out,
                        'json all listing mismatch')
        # List packets with notations
        params = ['--list-packets', data_path('test_key_edge_cases/key-critical-notations.pgp')]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*notation data: critical text = critical value.*$')
        self.assertRegex(out, r'(?s)^.*notation data: critical binary = 0x000102030405060708090a0b0c0d0e0f \(16 bytes\).*$')
        # List packets with notations via JSON
        params = ['--list-packets', '--json', data_path('test_key_edge_cases/key-critical-notations.pgp')]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*\"human\":true.*\"name\":\"critical text\".*\"value\":\"critical value\".*$')
        self.assertRegex(out, r'(?s)^.*\"human\":false.*\"name\":\"critical binary\".*\"value\":\"000102030405060708090a0b0c0d0e0f\".*$')
        # List test file with critical notation
        params = ['--list-packets', data_path('test_messages/message.txt.signed.crit-notation')]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*:type 20, len 35, critical.*notation data: critical text = critical value.*$')
        # List signature with signer's userid subpacket
        params = ['--list-packets', data_path(MSG_SIG_CRCR)]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*:type 28, len 9.*signer\'s user ID: alice@rnp.*$')
        # JSON list signature with signer's userid subpacket
        params = ['--list-packets', '--json', data_path(MSG_SIG_CRCR)]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*"type.str":"signer\'s user ID".*"length":9.*"uid":"alice@rnp".*$')
        # List signature with reason for revocation subpacket
        KEY = data_path('test_uid_validity/key-sig-revocation.pgp')
        params = ['--list-packets', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*:type 29, len 24.*reason for revocation: 32 \(No longer valid\).*message: Testing revoked userid.*$')
        # JSON list signature with reason for revocation subpacket
        params = ['--list-packets', '--json', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*"type.str":"reason for revocation".*"code":32.*"message":"Testing revoked userid.".*$')
        # v5 public key
        KEY = data_path('test_stream_key_load/v5-rsa-pub.asc')
        params = ['--list-packets', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*v5 public key material length: 391.*v5 public key material length: 391.*$')
        self.assertNotRegex(out, r'(?s)^.*v5 s2k length.*$')
        self.assertNotRegex(out, r'(?s)^.*v5 secret key data length.*$')
        params = ['--list-packets', '--json', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*"v5 public key material length":391.*"v5 public key material length":391.*$')
        self.assertNotRegex(out, r'(?s)^.*"v5 s2k length".*$')
        self.assertNotRegex(out, r'(?s)^.*"v5 secret key data length".*$')
        # v5 secret key
        KEY = data_path('test_stream_key_load/v5-rsa-sec.asc')
        params = ['--list-packets', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*v5 s2k length: 28.*v5 secret key data length: 988.*v5 s2k length: 28.*v5 secret key data length: 987.*$')
        params = ['--list-packets', '--json', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*"v5 s2k length":28.*"v5 secret key data length":988.*"v5 s2k length":28.*"v5 secret key data length":987.*$')
        # key with designated revoker packet
        KEY = data_path('test_stream_key_load/ecc-p256-desigrevoked-25519-pub.asc')
        params = ['--list-packets', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*revocation key.*class.*128.*$')
        params = ['--list-packets', '--json', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*"revocation key".*"class":128.*$')
        # primary userid subpackets
        KEY = data_path('test_uid_validity/key-uids-pub.pgp')
        params = ['--list-packets', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.* primary user ID: 1.*$')
        params = ['--list-packets', '--json', KEY]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*"primary":true.*$')

    def test_rnp_list_packets_edge_cases(self):
        KEY_EMPTY_UID = data_path('test_key_edge_cases/key-empty-uid.pgp')
        # List empty key packets
        params = ['--list-packets', data_path('test_key_edge_cases/key-empty-packets.pgp')]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_key_edge_cases/key-empty-packets.txt'), out,
                        'key-empty-packets listing mismatch')

        # List empty key packets json
        params = ['--list-packets', '--json', data_path('test_key_edge_cases/key-empty-packets.pgp')]
        ret, _, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)

        # List empty uid
        params = ['--list-packets', KEY_EMPTY_UID]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_key_edge_cases/key-empty-uid.txt'), out,
                        'key-empty-uid listing mismatch')

        # List empty uid with raw packet contents
        params = ['--list-packets', '--raw', KEY_EMPTY_UID]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_key_edge_cases/key-empty-uid-raw.txt'), out,
                        'key-empty-uid-raw listing mismatch')

        # List empty uid packet contents to JSON
        params = ['--list-packets', '--json', KEY_EMPTY_UID]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_key_edge_cases/key-empty-uid.json'), out,
                        'key-empty-uid json listing mismatch')

        # List experimental subpackets
        params = ['--list-packets', data_path('test_key_edge_cases/key-subpacket-101-110.pgp')]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_key_edge_cases/key-subpacket-101-110.txt'), out,
                        'key-subpacket-101-110 listing mismatch')

        # List experimental subpackets JSON
        params = ['--list-packets', '--json', data_path('test_key_edge_cases/key-subpacket-101-110.pgp')]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_key_edge_cases/key-subpacket-101-110.json'), out,
                        'key-subpacket-101-110 json listing mismatch')

        # List malformed signature
        params = ['--list-packets', data_path('test_key_edge_cases/key-malf-sig.pgp')]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_key_edge_cases/key-malf-sig.txt'), out,
                        'key-malf-sig listing mismatch')

        # List malformed signature JSON
        params = ['--list-packets', '--json', data_path('test_key_edge_cases/key-malf-sig.pgp')]
        ret, out, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, PKT_LIST_FAILED)
        compare_file_ex(data_path('test_key_edge_cases/key-malf-sig.json'), out,
                        'key-malf-sig json listing mismatch')

    def test_debug_log(self):
        if RNP_CAST5:
            run_proc(RNPK, ['--homedir', data_path(KEYRING_DIR_1), '--list-keys', '--debug', '--all'])
        run_proc(RNPK, ['--homedir', data_path(KEYRING_DIR_2), '--list-keys', '--debug', '--all'])
        run_proc(RNPK, ['--homedir', data_path(KEYRING_DIR_3), '--list-keys', '--debug', '--all'])
        run_proc(RNPK, ['--homedir', data_path(SECRING_G10),
                        '--list-keys', '--debug', '--all'])

    def test_pubring_loading(self):
        NO_PUBRING = r'(?s)^.*warning: keyring at path \'.*/pubring.gpg\' doesn\'t exist.*$'
        EMPTY_HOME = r'(?s)^.*Keyring directory .* is empty.*rnpkeys.*GnuPG.*'
        NO_USERID = 'No userid or default key for operation'

        test_dir = tempfile.mkdtemp(prefix='rnpctmp')
        test_data = data_path(MSG_TXT)
        output = os.path.join(test_dir, 'output')
        params = ['--symmetric', '--password', 'pass', '--homedir', test_dir, test_data, '--output', output]
        ret, _, err = run_proc(RNP, ['--encrypt'] + params)
        self.assertEqual(ret, 1, 'encrypt w/o pubring didn\'t fail')
        self.assertNotRegex(err, NO_PUBRING, 'wrong no-keyring message')
        self.assertRegex(err, EMPTY_HOME)
        self.assertIn(NO_USERID, err, 'Unexpected no key output')
        self.assertIn('Failed to build recipients key list', err, 'Unexpected key list output')

        ret, _, err = run_proc(RNP, ['--sign'] + params)
        self.assertEqual(ret, 1, 'sign w/o pubring didn\'t fail')
        self.assertNotRegex(err, NO_PUBRING, 'wrong failure output')
        self.assertRegex(err, EMPTY_HOME)
        self.assertIn(NO_USERID, err, 'wrong no userid message')
        self.assertIn('Failed to build signing keys list', err, 'wrong signing list failure message')

        ret, _, err = run_proc(RNP, ['--clearsign'] + params)
        self.assertEqual(ret, 1, 'clearsign w/o pubring didn\'t fail')
        self.assertNotRegex(err, NO_PUBRING, 'wrong clearsign no pubring message')
        self.assertRegex(err, EMPTY_HOME)
        self.assertIn(NO_USERID, err, 'Unexpected clearsign no key output')
        self.assertIn('Failed to build signing keys list', err, 'Unexpected clearsign key list output')

        ret, _, _ = run_proc(RNP, params)
        self.assertEqual(ret, 0, 'symmetric w/o pubring failed')

        shutil.rmtree(test_dir)

    def test_homedir_accessibility(self):
        ret, _, err = run_proc(RNPK, ['--homedir', os.path.join(RNPDIR, 'non-existing'), '--generate', '--password=none'])
        self.assertNotEqual(ret, 0, 'failed to check for homedir accessibility')
        self.assertRegex(err, r'(?s)^.*Home directory .*.rnp.non-existing.* does not exist or is not writable!')
        self.assertRegex(err, RE_KEYSTORE_INFO)
        os.mkdir(os.path.join(RNPDIR, 'existing'), 0o700)
        ret, _, err = run_proc(RNPK, ['--homedir', os.path.join(RNPDIR, 'existing'), '--generate', '--password=none'])
        self.assertEqual(ret, 0, 'failed to use writeable and existing homedir')
        self.assertNotRegex(err, r'(?s)^.*Home directory .* does not exist or is not writable!')
        self.assertNotRegex(err, RE_KEYSTORE_INFO)

    def test_no_home_dir(self):
        del os.environ['HOME']
        ret, _, err = run_proc(RNP, ['-v', 'non-existing.pgp'])
        self.assertEqual(ret, 2, 'failed to run without HOME env variable')
        self.assertRegex(err, r'(?s)^.*Home directory .* does not exist or is not writable!')
        self.assertRegex(err, RE_KEYSTORE_INFO)

    def test_exit_codes(self):
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--help'])
        self.assertEqual(ret, 0, 'invalid exit code of \'rnp --help\'')
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--help'])
        self.assertEqual(ret, 0, 'invalid exit code of \'rnpkeys --help\'')
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--unknown-option', '--help'])
        self.assertNotEqual(ret, 0, 'rnp should return non-zero exit code for unknown command line options')
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--unknown-option', '--help'])
        self.assertNotEqual(ret, 0, 'rnpkeys should return non-zero exit code for unknown command line options')

    def test_input_from_specifier(self):
        KEY_LIST = r'(?s)^.*' \
        r'1 key found.*' \
        r'pub .*255/EdDSA.*0451409669ffde3c.*' \
        r'73edcc9119afc8e2dbbdcde50451409669ffde3c.*$'
        NO_KEY_LIST = r'(?s)^.*' \
        r'Key\(s\) not found.*$'
        WRONG_VAR = r'(?s)^.*' \
        r'Failed to get value of the environment variable \'SOMETHING_UNSET\'.*' \
        r'Failed to create input for env:SOMETHING_UNSET.*$'
        WRONG_DATA = r'(?s)^.*' \
        r'failed to import key\(s\) from env:KEY_FILE, stopping.*$'
        PGP_MSG = r'(?s)^.*' \
        r'-----BEGIN PGP MESSAGE-----.*' \
        r'-----END PGP MESSAGE-----.*$'
        ENV_KEY = 'env:KEY_FILE'

        clear_keyrings()
        # Import key from the stdin
        ktext = file_text(data_path(KEY_ALICE_SEC))
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', '-'], ktext)
        self.assertEqual(ret, 0, 'failed to import key from stdin')
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0, KEY_LIST_FAILED)
        self.assertRegex(out, KEY_LIST, KEY_LIST_WRONG)
        # Cleanup and import key from the env variable
        clear_keyrings()
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertNotEqual(ret, 0, 'no key list failed')
        self.assertRegex(out, NO_KEY_LIST, KEY_LIST_WRONG)
        # Pass unset variable
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', 'env:SOMETHING_UNSET'])
        self.assertNotEqual(ret, 0, 'key import from env must fail')
        self.assertRegex(err, WRONG_VAR, 'wrong output')
        # Pass incorrect value in environment variable
        os.environ['KEY_FILE'] = "something"
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', ENV_KEY])
        self.assertNotEqual(ret, 0, 'key import failed')
        self.assertRegex(err, WRONG_DATA, 'wrong output')
        # Now import the correct key
        os.environ['KEY_FILE'] = ktext
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', ENV_KEY])
        self.assertEqual(ret, 0, 'key import failed')
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0, KEY_LIST_FAILED)
        self.assertRegex(out, KEY_LIST, KEY_LIST_WRONG)

        # Sign message from the stdin, using the env keyfile
        ret, out, _ = run_proc(RNP, ['-s', '-', '--password', 'password', '--armor', '--keyfile', ENV_KEY], 'Message to sign')
        self.assertEqual(ret, 0, 'Message signing failed')
        self.assertRegex(out, PGP_MSG, 'wrong signing output')
        os.environ['SIGN_MSG'] = out
        # Verify message from the env variable
        ret, out, _ = run_proc(RNP, ['-d', 'env:SIGN_MSG', '--keyfile', ENV_KEY])
        self.assertEqual(ret, 0, 'Message verification failed')
        self.assertEqual(out, 'Message to sign', 'wrong verification output')

    def test_output_to_specifier(self):
        src, enc, encasc, dec = reg_workfiles('source', '.txt', EXT_PGP, EXT_ASC, '.dec')
        with open(src, 'w+') as f:
            f.write('Hello world')
        # Encrypt file and make sure result is stored with .pgp extension
        ret, out, _ = run_proc(RNP, ['-c', src, '--password', 'password'])
        self.assertEqual(ret, 0, ENC_FAILED)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '-d', enc, '--output', dec, '--password', 'password'])
        self.assertEqual(ret, 0, DEC_FAILED)
        self.assertEqual(file_text(src), file_text(dec), DEC_DIFFERS)
        remove_files(enc, dec)
        # Encrypt file with armor and make sure result is stored with .asc extension
        ret, _, _ = run_proc(RNP, ['-c', src, '--armor', '--password', 'password'])
        self.assertEqual(ret, 0, ENC_FAILED)
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '-d', encasc, '--output', '-', '--password', 'password'])
        self.assertEqual(ret, 0, DEC_FAILED)
        self.assertEqual(file_text(src), out, DEC_DIFFERS)
        remove_files(encasc)
        # Encrypt file and write result to the stdout
        ret, out, _ = run_proc(RNP, ['-c', src, '--armor', '--output', '-', '--password', 'password'])
        self.assertEqual(ret, 0, ENC_FAILED)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '-d', '--output', dec, '--password', 'password', '-'], out)
        self.assertEqual(ret, 0, DEC_FAILED)
        self.assertEqual(file_text(src), file_text(dec), DEC_DIFFERS)
        remove_files(dec)
        # Encrypt file and write armored result to the stdout
        ret, out, _ = run_proc(RNP, ['-c', src, '--armor','--output', '-', '--password', 'password'])
        self.assertEqual(ret, 0, ENC_FAILED)
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '-d', '--output', '-', '--password', 'password', '-'], out)
        self.assertEqual(ret, 0, DEC_FAILED)
        self.assertEqual(file_text(src), out, DEC_DIFFERS)
        # Encrypt stdin and write result to the stdout
        srctxt = file_text(src)
        ret, out, _ = run_proc(RNP, ['-c', '--armor', '--password', 'password'], srctxt)
        self.assertEqual(ret, 0, ENC_FAILED)
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '-d', '--password', 'password'], out)
        self.assertEqual(ret, 0, DEC_FAILED)
        self.assertEqual(out, srctxt, DEC_DIFFERS)
        # Encrypt stdin and attempt to write to non-existing dir
        ret, _, err = run_proc(RNP, ['-c', '--armor', '--password', 'password', '--output', 'nonexisting/output.pgp'], srctxt)
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*init_file_dest.*failed to create file.*output.pgp.*Error 2.*$')
        self.assertNotRegex(err, r'(?s)^.*failed to initialize encryption.*$')
        self.assertRegex(err, r'(?s)^.*failed to open source or create output.*$')
        # Sign stdin and then verify it using non-existing directory for output
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '--armor', '--password', 'password', '-s'], srctxt)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*BEGIN PGP MESSAGE.*END PGP MESSAGE.*$')
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '-v', '--output', 'nonexisting/output.pgp'], out)
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*init_file_dest.*failed to create file.*output.pgp.*Error 2.*$')

    def test_literal_filename(self):
        EMPTY_FNAME = r'(?s)^.*literal data packet.*mode b.*created 0, name="".*$'
        HELLO_FNAME = r'(?s)^.*literal data packet.*mode b.*created 0, name="hello".*$'
        src, enc = reg_workfiles('source', '.txt', EXT_PGP)
        with open(src, 'w+') as f:
            f.write('Literal filename check')
        # Encrypt file and make sure it's name is stored in literal data packet
        ret, out, _ = run_proc(RNP, ['-c', src, '--password', 'password'])
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(GPG, ['--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', 'password', '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*literal data packet.*mode b.*created \d+.*name="source.txt".*$')
        remove_files(enc)
        # Encrypt file, overriding it's name
        ret, out, _ = run_proc(RNP, ['--set-filename', 'hello', '-c', src, '--password', 'password'])
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(GPG, ['--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', 'password', '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, HELLO_FNAME)
        remove_files(enc)
        # Encrypt file, using empty name
        ret, out, _ = run_proc(RNP, ['--set-filename', '', '-c', src, '--password', 'password'])
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(GPG, ['--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', 'password', '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, EMPTY_FNAME)
        remove_files(enc)
        # Encrypt stdin, making sure empty name is stored
        ret, out, _ = run_proc(RNP, ['-c', '--password', 'password', '--output', enc], 'Data from stdin')
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(GPG, ['--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', 'password', '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, EMPTY_FNAME)
        remove_files(enc)
        # Encrypt stdin, setting the file name
        ret, out, _ = run_proc(RNP, ['--set-filename', 'hello', '-c', '--password', 'password', '--output', enc], 'Data from stdin')
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(GPG, ['--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', 'password', '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, HELLO_FNAME)
        remove_files(enc)
        # Encrypt env, making sure empty name is stored
        ret, out, _ = run_proc(RNP, ['-c', 'env:HOME', '--password', 'password', '--output', enc])
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(GPG, ['--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', 'password', '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, EMPTY_FNAME)
        remove_files(enc)
        # Encrypt env, setting the file name
        ret, out, _ = run_proc(RNP, ['--set-filename', 'hello', '-c', 'env:HOME', '--password', 'password', '--output', enc])
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(GPG, ['--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', 'password', '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, HELLO_FNAME)
        remove_files(enc)

    def test_empty_keyrings(self):
        NO_KEYRING = r'(?s)^.*' \
        r'warning: keyring at path \'.*.\.rnp.pubring\.gpg\' doesn\'t exist.*' \
        r'warning: keyring at path \'.*.\.rnp.secring\.gpg\' doesn\'t exist.*$'
        EMPTY_KEYRING = r'(?s)^.*' \
        r'Warning: no keys were loaded from the keyring \'.*.\.rnp.pubring\.gpg\'.*' \
        r'Warning: no keys were loaded from the keyring \'.*.\.rnp.secring\.gpg\'.*$'
        PUB_IMPORT= r'(?s)^.*pub\s+255/EdDSA 0451409669ffde3c .* \[SC\].*$'
        EMPTY_SECRING = r'(?s)^.*Warning: no keys were loaded from the keyring \'.*\.rnp.secring.gpg\'.*$'
        SEC_IMPORT= r'(?s)^.*sec\s+255/EdDSA 0451409669ffde3c .* \[SC\].*$'
        EMPTY_HOME = r'(?s)^.*Keyring directory .* is empty.*rnpkeys.*GnuPG.*'

        os.rename(RNPDIR, RNPDIR + '-old')
        os.environ['HOME'] = WORKDIR
        try:
            self.assertFalse(os.path.isdir(RNPDIR), '.rnp directory should not exists')
            src, enc, dec = reg_workfiles('source', '.txt', EXT_PGP, '.dec')
            random_text(src, 2000)
            # Run symmetric encryption/decryption without .rnp home directory
            ret, _, err = run_proc(RNP, ['-c', src, '--password', 'password'])
            self.assertEqual(ret, 0, 'Symmetric encryption without home failed')
            self.assertNotRegex(err, NO_KEYRING, 'No keyring msg in encryption output')
            ret, _, err = run_proc(RNP, ['-d', enc, '--password', 'password', '--output', dec])
            self.assertEqual(ret, 0, 'Symmetric decryption without home failed')
            self.assertNotRegex(err, NO_KEYRING, 'No keyring msg in decryption output')
            self.assertRegex(err, EMPTY_HOME)
            self.assertIn(WORKDIR, err, 'No workdir in decryption output')
            compare_files(src, dec, DEC_DIFFERS)
            remove_files(enc, dec)
            # Import key without .rnp home directory
            ret, out, err = run_proc(RNPK, ['--import', data_path(KEY_ALICE_PUB)])
            self.assertEqual(ret, 0, 'Key import failed without home')
            self.assertNotRegex(err, NO_KEYRING, 'No keyring msg in key import output')
            self.assertRegex(err, EMPTY_HOME)
            self.assertIn(WORKDIR, err, 'No workdir in key import output')
            self.assertRegex(out, PUB_IMPORT, 'Wrong key import output')
            ret, out, err = run_proc(RNPK, ['--import', data_path(KEY_ALICE_SEC)])
            self.assertEqual(ret, 0, 'Secret key import without home failed')
            self.assertNotRegex(err, NO_KEYRING, 'no keyring message in key import output')
            self.assertNotRegex(err, EMPTY_HOME)
            self.assertRegex(err, EMPTY_SECRING, 'no empty secring in key import output')
            self.assertIn(WORKDIR, err, 'no workdir in key import output')
            self.assertRegex(out, SEC_IMPORT, 'Wrong secret key import output')
            # Run with empty .rnp home directory
            shutil.rmtree(RNPDIR, ignore_errors=True)
            os.mkdir(RNPDIR, 0o700)
            ret, _, err = run_proc(RNP, ['-c', src, '--password', 'password'])
            self.assertEqual(ret, 0)
            self.assertNotRegex(err, NO_KEYRING)
            ret, out, err = run_proc(RNP, ['-d', enc, '--password', 'password', '--output', dec])
            self.assertEqual(ret, 0, 'Symmetric decryption failed')
            self.assertRegex(err, EMPTY_HOME)
            self.assertNotRegex(err, NO_KEYRING, 'No keyring message in decryption output')
            self.assertIn(WORKDIR, err, 'No workdir in decryption output')
            compare_files(src, dec, DEC_DIFFERS)
            remove_files(enc, dec)
            # Import key with empty .rnp home directory
            ret, out, err = run_proc(RNPK, ['--import', data_path(KEY_ALICE_PUB)])
            self.assertEqual(ret, 0, 'Public key import with empty home failed')
            self.assertNotRegex(err, NO_KEYRING, 'No keyring message in key import output')
            self.assertRegex(err, EMPTY_HOME)
            self.assertIn(WORKDIR, err, 'No workdir in key import output')
            self.assertRegex(out, PUB_IMPORT, 'Wrong pub key import output')
            ret, out, err = run_proc(RNPK, ['--import', data_path(KEY_ALICE_SEC)])
            self.assertEqual(ret, 0, 'Secret key import failed')
            self.assertNotRegex(err, NO_KEYRING, 'No-keyring message in secret key import output')
            self.assertRegex(err, EMPTY_SECRING, 'No empty secring msg in secret key import output')
            self.assertNotRegex(err, EMPTY_HOME)
            self.assertIn(WORKDIR, err, 'No workdir in secret key import output')
            self.assertRegex(out, SEC_IMPORT, 'wrong secret key import output')
            if not is_windows():
                # Attempt ro run with non-writable HOME
                newhome = os.path.join(WORKDIR, 'new')
                os.mkdir(newhome, 0o400)
                os.environ['HOME'] = newhome
                ret, out, err = run_proc(RNPK, ['--import', data_path(KEY_ALICE_PUB)])
                self.assertEqual(ret, 1)
                self.assertRegex(err, r'(?s)^.*Home directory \'.*new\' does not exist or is not writable!')
                self.assertRegex(err, RE_KEYSTORE_INFO)
                self.assertIn(WORKDIR, err)
                os.environ['HOME'] = WORKDIR
                shutil.rmtree(newhome, ignore_errors=True)
                # Attempt to load keyring with invalid permissions
                os.chmod(os.path.join(RNPDIR, PUBRING), 0o000)
                ret, out, err = run_proc(RNPK, ['--list-keys'])
                self.assertEqual(ret, 0)
                self.assertRegex(err, r'(?s)^.*Warning: failed to open keyring at path \'.*pubring\.gpg\' for reading.')
                self.assertRegex(out, r'(?s)^.*Alice <alice@rnp>')
                os.chmod(os.path.join(RNPDIR, SECRING), 0o000)
                ret, out, err = run_proc(RNPK, ['--list-keys'])
                self.assertEqual(ret, 1)
                self.assertRegex(err, r'(?s)^.*Warning: failed to open keyring at path \'.*pubring\.gpg\' for reading.')
                self.assertRegex(err, r'(?s)^.*Warning: failed to open keyring at path \'.*secring\.gpg\' for reading.')
                self.assertRegex(out, r'(?s)^.*Key\(s\) not found.')
            # Attempt to load keyring with random data
            shutil.rmtree(RNPDIR, ignore_errors=True)
            os.mkdir(RNPDIR, 0o700)
            random_text(os.path.join(RNPDIR, PUBRING), 1000)
            random_text(os.path.join(RNPDIR, SECRING), 1000)
            ret, out, err = run_proc(RNPK, ['--list-keys'])
            self.assertEqual(ret, 1)
            self.assertRegex(err, r'(?s)^.*Error: failed to load keyring from \'.*pubring\.gpg\'')
            self.assertNotRegex(err, r'(?s)^.*Error: failed to load keyring from \'.*secring\.gpg\'')
            # Run with .rnp home directory with empty keyrings
            shutil.rmtree(RNPDIR, ignore_errors=True)
            os.mkdir(RNPDIR, 0o700)
            random_text(os.path.join(RNPDIR, PUBRING), 0)
            random_text(os.path.join(RNPDIR, SECRING), 0)
            ret, out, err = run_proc(RNP, ['-c', src, '--password', 'password'])
            self.assertEqual(ret, 0, 'Symmetric encryption failed')
            self.assertNotRegex(err, EMPTY_KEYRING, 'Invalid encryption output')
            ret, out, err = run_proc(RNP, ['-d', enc, '--password', 'password', '--output', dec])
            self.assertEqual(ret, 0, 'Symmetric decryption failed')
            self.assertRegex(err, EMPTY_KEYRING, 'wrong decryption output')
            self.assertIn(WORKDIR, err, 'wrong decryption output')
            compare_files(src, dec, DEC_DIFFERS)
            remove_files(enc, dec)
            # Import key with empty keyrings in .rnp home directory
            ret, out, err = run_proc(RNPK, ['--import', data_path(KEY_ALICE_PUB)])
            self.assertEqual(ret, 0, 'Public key import failed')
            self.assertRegex(err, EMPTY_KEYRING, 'No empty keyring msg in key import output')
            self.assertIn(WORKDIR, err, 'No workdir in empty keyring key import output')
            self.assertRegex(out, PUB_IMPORT, 'Wrong pubkey import output')
            ret, out, err = run_proc(RNPK, ['--import', data_path(KEY_ALICE_SEC)])
            self.assertEqual(ret, 0, 'Secret key import failed')
            self.assertNotRegex(err, EMPTY_KEYRING, 'No empty keyring in key import output')
            self.assertRegex(err, EMPTY_SECRING, 'No empty secring in key import output')
            self.assertIn(WORKDIR, err, 'wrong key import output')
            self.assertRegex(out, SEC_IMPORT, 'wrong secret key import output')
        finally:
            shutil.rmtree(RNPDIR, ignore_errors=True)
            os.rename(RNPDIR + '-old', RNPDIR)

    def test_alg_aliases(self):
        src, enc = reg_workfiles('source', '.txt', EXT_PGP)
        with open(src, 'w+') as f:
            f.write('Hello world')
        # Encrypt file but forget to pass cipher name
        ret, _, err = run_proc(RNP, ['-c', src, '--password', 'password', '--cipher'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*rnp(|\.exe): option( .--cipher.|) requires an argument.*')
        # Encrypt file using the unknown symmetric algorithm
        ret, _, err = run_proc(RNP, ['-c', src, '--cipher', 'bad', '--password', 'password'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Unsupported encryption algorithm: bad.*$')
        # Encrypt file but forget to pass hash algorithm name
        ret, _, err = run_proc(RNP, ['-c', src, '--password', 'password', '--hash'])
        self.assertNotEqual(ret, 0)
        # Encrypt file using the unknown hash algorithm
        ret, _, err = run_proc(RNP, ['-c', src, '--hash', 'bad', '--password', 'password'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Unsupported hash algorithm: bad.*$')
        # Encrypt file using the AES algorithm instead of AES-128
        ret, _, err = run_proc(RNP, ['-c', src, '--cipher', 'AES', '--password', 'password'])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*Warning, unsupported encryption algorithm: AES.*$')
        self.assertNotRegex(err, r'(?s)^.*Unsupported encryption algorithm: AES.*$')
        # Make sure AES-128 was used
        ret, out, _ = run_proc(RNP, ['--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out,r'(?s)^.*Symmetric-key encrypted session key packet.*symmetric algorithm: 7 \(AES-128\).*$')
        remove_files(enc)
        # Encrypt file using the 3DES instead of tripledes
        ret, _, err = run_proc(RNP, ['-c', src, '--cipher', '3DES', '--password', 'password', "--allow-old-ciphers"])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*Warning, unsupported encryption algorithm: 3DES.*$')
        self.assertNotRegex(err, r'(?s)^.*Unsupported encryption algorithm: 3DES.*$')
        # Make sure 3DES was used
        ret, out, _ = run_proc(RNP, ['--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out,r'(?s)^.*Symmetric-key encrypted session key packet.*symmetric algorithm: 2 \(TripleDES\).*$')
        remove_files(enc)
        if RNP_RIPEMD160:
            # Use ripemd-160 hash instead of RIPEMD160
            ret, _, err = run_proc(RNP, ['-c', src, '--hash', 'ripemd-160', '--password', 'password'])
            self.assertEqual(ret, 0)
            self.assertNotRegex(err, r'(?s)^.*Unsupported hash algorithm: ripemd-160.*$')
            # Make sure RIPEMD160 was used
            ret, out, _ = run_proc(RNP, ['--list-packets', enc])
            self.assertEqual(ret, 0)
            self.assertRegex(out,r'(?s)^.*Symmetric-key encrypted session key packet.*s2k hash algorithm: 3 \(RIPEMD160\).*$')
            remove_files(enc)

    def test_core_dumps(self):
        CORE_DUMP = r'(?s)^.*warning: core dumps may be enabled, sensitive data may be leaked to disk.*$'
        NO_CORE_DUMP = r'(?s)^.*warning: --coredumps doesn\'t make sense on windows systems.*$'
        # Check rnpkeys for the message
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, CORE_DUMP)
        # Check rnp for the message
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--armor', '--password', 'password', '-c'], 'message')
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, CORE_DUMP)
        # Enable coredumps for rnpkeys
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--list-keys', '--coredumps'])
        self.assertEqual(ret, 0)
        if is_windows():
            self.assertNotRegex(err, CORE_DUMP)
            self.assertRegex(err, NO_CORE_DUMP)
        else:
            self.assertRegex(err, CORE_DUMP)
            self.assertNotRegex(err, NO_CORE_DUMP)
        # Enable coredumps for rnp
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--armor', '--password', 'password', '-c', '--coredumps'], 'message')
        self.assertEqual(ret, 0)
        if is_windows():
            self.assertNotRegex(err, CORE_DUMP)
            self.assertRegex(err, NO_CORE_DUMP)
        else:
            self.assertRegex(err, CORE_DUMP)
            self.assertNotRegex(err, NO_CORE_DUMP)

    def test_backend_version(self):
        BOTAN_BACKEND_VERSION = r'(?s)^.*.' \
        'Backend: Botan.*' \
        'Backend version: ([a-zA-Z\\.0-9]+).*$'
        OPENSSL_BACKEND_VERSION = r'(?s)^.*' \
        'Backend: OpenSSL.*' \
        'Backend version: ([a-zA-Z\\.0-9]+).*$'
        # Run without parameters and make sure it matches
        ret, out, _ = run_proc(RNP, [])
        self.assertNotEqual(ret, 0)
        match = re.match(BOTAN_BACKEND_VERSION, out) or re.match(OPENSSL_BACKEND_VERSION, out)
        self.assertTrue(match)
        # Run with version parameters
        ret, out, err = run_proc(RNP, ['--version'])
        self.assertEqual(ret, 0)
        match = re.match(BOTAN_BACKEND_VERSION, out)
        backend_prog = 'botan'
        if not match:
            match = re.match(OPENSSL_BACKEND_VERSION, out)
            backend_prog = 'openssl'
            openssl_root = os.getenv('RNP_TESTS_OPENSSL_ROOT')
        else:
            openssl_root = None
        self.assertTrue(match)
        # check there is no unexpected output
        self.assertNotRegex(err, r'(?is)^.*Unsupported.*$')
        self.assertNotRegex(err, r'(?is)^.*pgp_sa_to_openssl_string.*$')
        self.assertNotRegex(err, r'(?is)^.*unknown.*$')

        # In case when there are several openssl installations
        # testing environment is supposed to point to the right one
        # through OPENSSL_ROOT_DIR environment variable
        if is_windows():
            backend_prog += '.exe'
        backend_prog_ext = None
        if openssl_root is not None:
            backend_prog_ext = shutil.which(backend_prog, path = openssl_root + '/bin')
        else:
        # In all other cases
        # check that botan or openssl executable binary exists in PATH
            backend_prog_ext = shutil.which(backend_prog)

        if backend_prog_ext is None:
            return
        ret, out, _ = run_proc(backend_prog_ext, ['version'])
        self.assertEqual(ret, 0)
        self.assertIn(match.group(1), out)

    def test_help_message(self):
        # rnp help message
        # short -h option
        ret, out, _ = run_proc(RNP, ['-h'])
        self.assertEqual(ret, 0)
        short_h = out
        # long --help option
        ret, out, _ = run_proc(RNP, ['--help'])
        self.assertEqual(ret, 0)
        long_h = out
        self.assertEqual(short_h, long_h)
        # rnpkeys help message
        # short -h option
        ret, out, _ = run_proc(RNPK, ['-h'])
        self.assertEqual(ret, 0)
        short_h = out
        # long --help options
        ret, out, _ = run_proc(RNPK, ['--help'])
        self.assertEqual(ret, 0)
        long_h = out
        self.assertEqual(short_h, long_h)

    def test_wrong_mpi_bit_count(self):
        WRONG_MPI_BITS = r'(?s)^.*Warning! Wrong mpi bit count: got [0-9]+, but actual is [0-9]+.*$'
        # Make sure message is not displayed on normal keys
        ret, _, err = run_proc(RNP, ['--list-packets', data_path(PUBRING_1)])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, WRONG_MPI_BITS)
        # Make sure message is displayed on wrong mpi
        ret, _, err = run_proc(RNP, ['--list-packets', data_path('test_key_edge_cases/alice-wrong-mpi-bit-count.pgp')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, WRONG_MPI_BITS)

    def test_eddsa_small_x(self):
        os.rename(RNPDIR, RNPDIR + '-old')
        os.environ['HOME'] = WORKDIR
        try:
            self.assertFalse(os.path.isdir(RNPDIR), '.rnp directory should not exists')
            src, sig, ver = reg_workfiles('source', '.txt', EXT_PGP, '.dec')
            random_text(src, 2000)
            # load just public key and verify pre-signed message
            ret, _, _ = run_proc(RNPK, ['--import', data_path('test_key_edge_cases/key-eddsa-small-x-pub.asc')])
            self.assertEqual(ret, 0)
            ret, _, err = run_proc(RNP, ['--verify', data_path('test_messages/message.txt.sign-small-eddsa-x')])
            self.assertEqual(ret, 0)
            self.assertRegex(err, r'(?s)^.*Good signature made .*using EdDSA key 7bc55b9bdce36e18.*$')
            # load secret key and sign message
            ret, out, _ = run_proc(RNPK, ['--import', data_path('test_key_edge_cases/key-eddsa-small-x-sec.asc')])
            self.assertEqual(ret, 0)
            self.assertRegex(out, r'(?s)^.*sec.*255/EdDSA.*7bc55b9bdce36e18.*eddsa_small_x.*ssb.*c6c35ea115368a0b.*$')
            ret, _, _ = run_proc(RNP, ['--password', PASSWORD, '--sign', src, '--output', sig])
            self.assertEqual(ret, 0)
            # verify back
            ret, _, err = run_proc(RNP, ['--verify', sig, '--output', ver])
            self.assertEqual(ret, 0)
            self.assertEqual(file_text(src), file_text(ver))
            self.assertRegex(err, r'(?s)^.*Good signature made .*using EdDSA key 7bc55b9bdce36e18.*$')
            # verify back with GnuPG
            os.remove(ver)
            gpg_import_pubring(data_path('test_key_edge_cases/key-eddsa-small-x-pub.asc'))
            gpg_verify_file(sig, ver, 'eddsa_small_x')
        finally:
            shutil.rmtree(RNPDIR, ignore_errors=True)
            os.rename(RNPDIR + '-old', RNPDIR)

    def test_cv25519_bit_fix(self):
        RE_NOT_25519 = r'(?s)^.*Error: specified key is not Curve25519 ECDH subkey.*$'
        # Import and tweak non-protected secret key
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_25519_NOTWEAK_SEC)])
        self.assertEqual(ret, 0)
        # Check some --edit-key invalid options combinations
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*You need to specify a key or subkey to edit.*$')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '3176fc1486aa2528'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*You should specify one of the editing options for --edit-key.*$')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--check-cv25519-bits'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*You need to specify a key or subkey to edit.*$')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--check-cv25519-bits', 'key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Secret keys matching \'key\' not found.*$')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--check-cv25519-bits', 'eddsa-25519-non-tweaked'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, RE_NOT_25519)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--check-cv25519-bits', '3176fc1486aa2528'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, RE_NOT_25519)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--edit-key', '--check-cv25519-bits', '950ee0cd34613dba'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Warning: Cv25519 key bits need fixing.*$')
        # Tweak bits
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--fix-cv25519-bits', '3176fc1486aa2528'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, RE_NOT_25519)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--edit-key', '--fix-cv25519-bits', '950ee0cd34613dba'])
        self.assertEqual(ret, 0)
        # Make sure bits are correctly tweaked and key may be used to decrypt and imported to GnuPG
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--edit-key', '--check-cv25519-bits', '950ee0cd34613dba'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Cv25519 key bits are set correctly and do not require fixing.*$')
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '-d', data_path(MSG_ES_25519)])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import', os.path.join(RNPDIR, SECRING)])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '-d', data_path(MSG_ES_25519)])
        self.assertEqual(ret, 0)
        # Remove key
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--yes', '--delete-secret-key', 'dde0ee539c017d2bd3f604a53176fc1486aa2528'])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', '--force', 'dde0ee539c017d2bd3f604a53176fc1486aa2528'])
        self.assertEqual(ret, 0)
        # Make sure protected secret key works the same way
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_edge_cases/key-25519-non-tweaked-sec-prot.asc')])
        self.assertEqual(ret, 0)
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--password', 'wrong', '--edit-key', '--check-cv25519-bits', '950ee0cd34613dba'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Error: failed to unlock key. Did you specify valid password\\?.*$')
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', 'password', '--notty', '--edit-key', '--check-cv25519-bits', '950ee0cd34613dba'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Warning: Cv25519 key bits need fixing.*$')
        # Tweak bits
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--password', 'wrong', '--edit-key', '--fix-cv25519-bits', '950ee0cd34613dba'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Error: failed to unlock key. Did you specify valid password\\?.*$')
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--password', 'password', '--edit-key', '--fix-cv25519-bits', '950ee0cd34613dba'])
        self.assertEqual(ret, 0)
        # Make sure key is protected with the same options
        ret, out, _ = run_proc(RNP, ['--list-packets', os.path.join(RNPDIR, SECRING)])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Secret subkey packet.*254.*AES-256.*3.*SHA256.*58720256.*0x950ee0cd34613dba.*$')
        # Make sure bits are correctly tweaked and key may be used to decrypt and imported to GnuPG
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--password', 'password', '--edit-key', '--check-cv25519-bits', '950ee0cd34613dba'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Cv25519 key bits are set correctly and do not require fixing.*$')
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--password', 'password', '-d', data_path(MSG_ES_25519)])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--batch', '--passphrase', 'password', '--import', os.path.join(RNPDIR, SECRING)])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, GPG_LOOPBACK, '--batch', '--passphrase', 'password',
                                   '--trust-model', 'always', '-d', data_path(MSG_ES_25519)])
        self.assertEqual(ret, 0)
        # Remove key
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--yes', '--delete-secret-key', 'dde0ee539c017d2bd3f604a53176fc1486aa2528'])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', '--force', 'dde0ee539c017d2bd3f604a53176fc1486aa2528'])
        self.assertEqual(ret, 0)

    def test_aead_last_chunk_zero_length(self):
        # Cover case with last AEAD chunk of the zero size
        os.rename(RNPDIR, RNPDIR + '-old')
        os.mkdir(RNPDIR)
        try:
            dec, enc = reg_workfiles('cleartext', '.dec', '.enc')
            srctxt = data_path('test_messages/message.aead-last-zero-chunk.txt')
            srceax = data_path('test_messages/message.aead-last-zero-chunk.enc')
            srcocb = data_path('test_messages/message.aead-last-zero-chunk.enc-ocb')
            eax_size = os.path.getsize(srceax)
            ocb_size = os.path.getsize(srcocb)
            self.assertEqual(eax_size - 1, ocb_size)
            # Import Alice's key
            ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_ALICE_SUB_SEC)])
            self.assertEqual(ret, 0)
            # Decrypt already existing file
            if RNP_AEAD_EAX and RNP_BRAINPOOL:
                ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '-d', srceax, '--output', dec])
                self.assertEqual(ret, 0)
                self.assertEqual(file_text(srctxt), file_text(dec))
                os.remove(dec)
            if RNP_AEAD_OCB and RNP_BRAINPOOL:
                ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '-d', srcocb, '--output', dec])
                self.assertEqual(ret, 0)
                self.assertEqual(file_text(srctxt), file_text(dec))
                os.remove(dec)
            # Decrypt with gnupg
            if GPG_AEAD and GPG_BRAINPOOL:
                ret, _, _ = run_proc(GPG, ['--batch', '--passphrase', PASSWORD, '--homedir',
                                        GPGHOME, '--import', data_path(KEY_ALICE_SUB_SEC)])
                self.assertEqual(ret, 0, GPG_IMPORT_FAILED)
                if GPG_AEAD_EAX:
                    gpg_decrypt_file(srceax, dec, PASSWORD)
                    self.assertEqual(file_text(srctxt), file_text(dec))
                    os.remove(dec)
                if GPG_AEAD_OCB:
                    gpg_decrypt_file(srcocb, dec, PASSWORD)
                    self.assertEqual(file_text(srctxt), file_text(dec))
                    os.remove(dec)
            if RNP_AEAD_EAX and RNP_BRAINPOOL:
                # Encrypt with RNP
                ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '-z', '0', '-r', 'alice', '--aead=eax',
                                           '--set-filename', 'cleartext-z0.txt', '--aead-chunk-bits=1', '-e', srctxt, '--output', enc])
                self.assertEqual(ret, 0)
                self.assertEqual(os.path.getsize(enc), eax_size)
                # Decrypt with RNP again
                ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '-d', enc, '--output', dec])
                self.assertEqual(file_text(srctxt), file_text(dec))
                os.remove(dec)
                if GPG_AEAD_EAX and GPG_BRAINPOOL:
                    # Decrypt with GnuPG
                    gpg_decrypt_file(enc, dec, PASSWORD)
                    self.assertEqual(file_text(srctxt), file_text(dec))
                os.remove(enc)
            if RNP_AEAD_OCB and RNP_BRAINPOOL:
                # Encrypt with RNP
                ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '-z', '0', '-r', 'alice', '--aead=ocb',
                                           '--set-filename', 'cleartext-z0.txt', '--aead-chunk-bits=1', '-e', srctxt, '--output', enc])
                self.assertEqual(ret, 0)
                self.assertEqual(os.path.getsize(enc), ocb_size)
                # Decrypt with RNP again
                ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '-d', enc, '--output', dec])
                self.assertEqual(file_text(srctxt), file_text(dec))
                os.remove(dec)
                if GPG_AEAD_OCB and GPG_BRAINPOOL:
                    # Decrypt with GnuPG
                    gpg_decrypt_file(enc, dec, PASSWORD)
                    self.assertEqual(file_text(srctxt), file_text(dec))
        finally:
            shutil.rmtree(RNPDIR, ignore_errors=True)
            os.rename(RNPDIR + '-old', RNPDIR)

    def test_text_sig_crcr(self):
        # Cover case with line ending with multiple CRs
        srcsig = data_path(MSG_SIG_CRCR)
        srctxt = data_path('test_messages/message.text-sig-crcr')
        # Verify with RNP
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', data_path(KEY_ALICE_SUB_PUB), '-v', srcsig])
        self.assertEqual(ret, 0)
        # Verify with GPG
        if GPG_BRAINPOOL:
            ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import', data_path(KEY_ALICE_SUB_PUB)])
            self.assertEqual(ret, 0, GPG_IMPORT_FAILED)
            gpg_verify_detached(srctxt, srcsig, KEY_ALICE)

    def test_encrypted_password_wrong(self):
        # Test symmetric decryption with wrong password used
        srcenc = data_path('test_messages/message.enc-password')
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', 'password1', '-d', srcenc])
        self.assertNotEqual(ret, 0)
        self.assertIn('checksum check failed', err)
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', 'password', '-d', srcenc, '--output', 'decrypted'])
        self.assertEqual(ret, 0)
        os.remove('decrypted')

    def test_clearsign_long_lines(self):
        # Cover case with cleartext signed file with long lines and filesize > 32k (buffer size)
        [sig] = reg_workfiles('cleartext', '.sig')
        srctxt = data_path('test_messages/message.4k-long-lines')
        srcsig = data_path('test_messages/message.4k-long-lines.asc')
        pubkey = data_path(KEY_ALICE_SUB_PUB)
        seckey = data_path(KEY_ALICE_SUB_SEC)
        # Verify already existing file
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', pubkey, '-v', srcsig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*73edcc9119afc8e2dbbdcde50451409669ffde3c.*')
        # Verify with gnupg
        if GPG_BRAINPOOL:
            ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import', pubkey])
            self.assertEqual(ret, 0, GPG_IMPORT_FAILED)
            gpg_verify_cleartext(srcsig, KEY_ALICE)
        # Sign again with RNP
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', seckey, '--password', PASSWORD, '--clearsign', srctxt, '--output', sig])
        self.assertEqual(ret, 0)
        # Verify with RNP again
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*73edcc9119afc8e2dbbdcde50451409669ffde3c.*')
        # Verify with gnupg again
        if GPG_BRAINPOOL:
            gpg_verify_cleartext(sig, KEY_ALICE)

    def test_eddsa_sig_lead_zero(self):
        # Cover case with lead zeroes in EdDSA signature
        srcs = data_path('test_messages/eddsa-zero-s.txt.sig')
        srcr = data_path('test_messages/eddsa-zero-r.txt.sig')
        # Verify with RNP
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', data_path(KEY_ALICE_SUB_PUB), '-v', srcs])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', data_path(KEY_ALICE_SUB_PUB), '-v', srcr])
        self.assertEqual(ret, 0)
        # Verify with GPG
        if GPG_BRAINPOOL:
            [dst] = reg_workfiles('eddsa-zero', '.txt')
            ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import', data_path(KEY_ALICE_SUB_PUB)])
            self.assertEqual(ret, 0, GPG_IMPORT_FAILED)
            gpg_verify_file(srcs, dst, KEY_ALICE)
            os.remove(dst)
            gpg_verify_file(srcr, dst, KEY_ALICE)

    def test_eddsa_seckey_lead_zero(self):
        # Load and use *unencrypted* EdDSA secret key with 2 leading zeroes
        seckey = data_path('test_stream_key_load/eddsa-00-sec.pgp')
        pubkey = data_path('test_stream_key_load/eddsa-00-pub.pgp')
        src, sig = reg_workfiles('source', '.txt', '.sig')
        random_text(src, 2000)

        # Sign with RNP
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', seckey, '-s', src, '--output', sig])
        self.assertEqual(ret, 0)
        # Verify with RNP
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        # Verify with GnuPG
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import', pubkey])
        ret, _, err = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--verify', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Signature made.*8BF2223370F61F8D965B.*Good signature from "eddsa-lead-zero".*$')

    def test_verify_detached_source(self):
        if not RNP_CAST5:
            self.skipTest("CAST5 is not supported")
        # Test --source parameter for the detached signature verification.
        src = data_path(MSG_TXT)
        sig = data_path(MSG_TXT + '.sig')
        sigasc = data_path(MSG_TXT + '.asc')
        keys = data_path(KEYRING_DIR_1)
        # Just verify
        ret, _, err = run_proc(RNP, ['--homedir', keys, '-v', sig])
        self.assertEqual(ret, 0)
        R_GOOD = r'(?s)^.*Good signature made.*e95a3cbf583aa80a2ccc53aa7bc6709b15c23a4a.*'
        self.assertRegex(err, R_GOOD)
        # Verify .asc
        ret, _, err = run_proc(RNP, ['--homedir', keys, '-v', sigasc])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD)
        # Do not provide source
        ret, _, err = run_proc(RNP, ['--homedir', keys, '-v', sig, '--source'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*rnp(|\.exe): option( .--source.|) requires an argument.*')
        # Verify by specifying the correct path
        ret, _, err = run_proc(RNP, ['--homedir', keys, '--source', src, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD)
        # Verify by specifying the incorrect path
        ret, _, err = run_proc(RNP, ['--homedir', keys, '--source', src + '.wrong', '-v', sig])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Failed to open source for detached signature verification.*')
        # Verify detached signature with non-asc/sig extension
        [csig] = reg_workfiles('message', '.dat')
        shutil.copy(sig, csig)
        ret, _, err = run_proc(RNP, ['--homedir', keys, '-v', csig])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Unsupported detached signature extension. Use --source to override.*')
        # Verify by reading data from stdin
        srcdata = ""
        with open(src, "rb") as srcf:
            srcdata = srcf.read().decode('utf-8')
        ret, _, err = run_proc(RNP, ['--homedir', keys, '--source', '-', '-v', csig], srcdata)
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD)
        # Verify by reading data from env
        os.environ["SIGNED_DATA"] = srcdata
        ret, _, err = run_proc(RNP, ['--homedir', keys, '--source', 'env:SIGNED_DATA', '-v', csig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD)
        del os.environ["SIGNED_DATA"]
        # Attempt to verify by specifying bot sig and data from stdin
        sigtext = file_text(sigasc)
        ret, _, err = run_proc(RNP, ['--homedir', keys, '--source', '-', '-v'], sigtext)
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Detached signature and signed source cannot be both stdin.*')

    def test_onepass_edge_cases(self):
        key = data_path('test_key_validity/alice-pub.asc')
        onepass22 = data_path('test_messages/message.txt.signed-2-2-onepass-v10')
        # Verify one-pass which doesn't match the signature - different keyid
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.signed-wrong-onepass')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Warning: signature doesn\'t match one-pass.*Good signature made.*0451409669ffde3c.*')
        # Verify one-pass with unknown hash algorithm
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.signed-unknown-onepass-hash')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Failed to create hash 136 for onepass 0.*')
        # Verify one-pass with hash algorithm which doesn't match sig's one
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.signed-wrong-onepass-hash')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*failed to get hash context.*BAD signature.*0451409669ffde3c.*')
        # Extra one-pass without the corresponding signature
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.signed-2-onepass')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Warning: premature end of signatures.*Good signature made.*0451409669ffde3c.*')
        # Two one-passes and two equal signatures
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.signed-2-2-onepass')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*0451409669ffde3c.*Good signature made.*0451409669ffde3c.*')
        # Two one-passes and two sigs, but first one-pass is of unknown version
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', onepass22])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*wrong packet version.*warning: unexpected data on the stream end.*Good signature made.*0451409669ffde3c.*')
        # Dump it as well
        ret, out, err = run_proc(RNP, ['--list-packets', onepass22])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*wrong packet version.*failed to process packet.*')
        self.assertRegex(out, r'(?s)^.*:off 0: packet header 0xc40d.*:off 15: packet header 0xc40d.*One-pass signature packet.*')
        # Dump it in JSON
        ret, out, err = run_proc(RNP, ['--list-packets', '--json', onepass22])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*wrong packet version.*failed to process packet.*')
        self.assertRegex(out, r'(?s)^.*"offset":0.*"tag":4.*"offset":15.*"tag":4.*"version":3.*"nested":true.*')
        # Two one-passes and sig of the unknown version
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.signed-2-2-sig-v10')])
        self.assertEqual(ret, 1)
        R_VER_10 = r'(?s)^.*unknown signature version: 10.*failed to parse signature.*UNKNOWN signature.*Good signature made.*0451409669ffde3c.*'
        R_1_UNK = r'(?s)^.*Signature verification failure: 1 unknown signature.*'
        self.assertRegex(err, R_VER_10)
        self.assertRegex(err, R_1_UNK)
        # Two one-passes and sig of the unknown version (second)
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.signed-2-2-sig-v10-2')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*unknown signature version: 10.*failed to parse signature.*Good signature made.*0451409669ffde3c.*UNKNOWN signature.*')
        self.assertRegex(err, R_1_UNK)
        # 2 detached signatures, first is of version 10
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.2sigs'), '--source', data_path(MSG_TXT)])
        self.assertEqual(ret, 1)
        self.assertRegex(err, R_VER_10)
        self.assertRegex(err, R_1_UNK)
        # 2 detached signatures, second is of version 10
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.2sigs-2'), '--source', data_path(MSG_TXT)])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*unknown signature version: 10.*failed to parse signature.*Good signature made.*0451409669ffde3c.*UNKNOWN signature.*')
        self.assertRegex(err, R_1_UNK)
        # Two cleartext signatures, first is of unknown version
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.clear-2-sigs')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, R_VER_10)
        self.assertRegex(err, R_1_UNK)
        # Two cleartext signatures, second is of unknown version
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-v', data_path('test_messages/message.txt.clear-2-sigs-2')])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*unknown signature version: 11.*failed to parse signature.*Good signature made.*0451409669ffde3c.*UNKNOWN signature.*')
        self.assertRegex(err, R_1_UNK)

    def test_pkesk_skesk_wrong_version(self):
        key = data_path('test_stream_key_load/ecc-p256-sec.asc')
        msg = data_path('test_messages/message.txt.pkesk-skesk-v10')
        msg2 = data_path('test_messages/message.txt.pkesk-skesk-v10-only')
        # Decrypt with secret key
        ret, out, err = run_proc(RNP, ['--keyfile', key, '--password', PASSWORD, '-d', msg])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*This is test message to be signed, and/or encrypted, cleartext signed and detached signed.*')
        self.assertRegex(err, r'(?s)^.*wrong packet version.*Failed to parse PKESK, skipping.*wrong packet version.*Failed to parse SKESK, skipping.*')
        # Decrypt with password
        ret, out, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '-d', msg])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*This is test message to be signed, and/or encrypted, cleartext signed and detached signed.*')
        self.assertRegex(err, r'(?s)^.*wrong packet version.*Failed to parse PKESK, skipping.*wrong packet version.*Failed to parse SKESK, skipping.*')
        # Attempt to decrypt message with only invalid PKESK/SKESK
        ret, _, err = run_proc(RNP, ['--keyfile', key, '--password', PASSWORD, '-d', msg2])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*wrong packet version.*Failed to parse PKESK, skipping.*wrong packet version.*Failed to parse SKESK, skipping.*failed to obtain decrypting key or password.*')

    def test_ext_adding_stripping(self):
        # Check whether rnp correctly strip .pgp/.gpg/.asc extension
        seckey = data_path('test_stream_key_load/ecc-p256-sec.asc')
        pubkey = data_path('test_stream_key_load/ecc-p256-pub.asc')
        src, src2, asc, pgp, gpg, some = reg_workfiles('cleartext', '.txt', '.txt2', '.txt.asc', '.txt.pgp', '.txt.gpg', '.txt.some')
        with open(src, 'w+') as f:
            f.write('Hello world')
        # Encrypt with binary output
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', pubkey, '-e', src])
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(pgp))
        # Decrypt binary output, it must be put in cleartext.txt if it doesn't exists
        os.remove(src)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', seckey, '--password', PASSWORD, '-d', pgp])
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(src))
        # Decrypt binary output with the rename prompt
        ret, out, _ = run_proc(RNP, ['--keyfile', seckey, '--password', PASSWORD, '--notty', '-d', pgp], "n\n" + src2 + "\n")
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(src2))
        self.assertRegex(out, r'(?s)^.*File.*cleartext.txt.*already exists. Would you like to overwrite it.*Please enter the new filename:.*$')
        self.assertIn(src, out)
        self.assertTrue(os.path.isfile(src2))
        os.remove(src2)
        # Rename from .pgp to .gpg and try again
        os.remove(src)
        os.rename(pgp, gpg)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', seckey, '--password', PASSWORD, '-d', gpg])
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(src))
        # Rename from .pgp to .some and check that all is put in stdout
        os.rename(gpg, some)
        ret, out, _ = run_proc(RNP, ['--keyfile', seckey, '--password', PASSWORD, '--notty', '-d', some], "\n\n")
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^\s*Hello world\s*$')
        os.remove(some)
        # Encrypt with armored output
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', pubkey, '-e', src, '--armor'])
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(asc))
        # Decrypt armored output, it must be put in cleartext.txt if it doesn't exists
        os.remove(src)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--keyfile', seckey, '--password', PASSWORD, '-d', asc])
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(src))
        # Enarmor - must be put in .asc file
        os.remove(asc)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--enarmor=msg', src])
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(asc))
        # Dearmor asc - must be outputted to src
        os.remove(src)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--dearmor', asc])
        self.assertEqual(ret, 0)
        self.assertTrue(os.path.isfile(src))
        # Dearmor unknown extension - must be put to stdout
        os.rename(asc, some)
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '--dearmor', some])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^\s*Hello world\s*$')


    def test_interactive_password(self):
        # Reuse password for subkey, say "yes"
        stdinstr = 'password\npassword\ny\n'
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--generate-key'], stdinstr)
        self.assertEqual(ret, 0)
        # Do not reuse same password for subkey, say "no"
        stdinstr = 'password\npassword\nN\nsubkeypassword\nsubkeypassword\n'
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--generate-key'], stdinstr)
        self.assertEqual(ret, 0)
        # Set empty password and reuse it
        stdinstr = '\n\ny\ny\n'
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--notty', '--generate-key'], stdinstr)
        self.assertEqual(ret, 0)

    def test_set_current_time(self):
        # Too old date
        is64bit = sys.maxsize > 2 ** 32
        gparam = ['--homedir', RNPDIR2, '--notty', '--password', PASSWORD, '--generate-key', '--numbits', '1024', '--current-time']
        rparam = ['--homedir', RNPDIR2, '--notty', '--remove-key']
        ret, out, err = run_proc(RNPK, gparam + ['1950-01-02', '--userid', 'key-1950'])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*invalid date: 1950-01-02.*$')
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*$')
        ret, _, _ = run_proc(RNPK, rparam + ['key-1950'], 'y\n')
        self.assertEqual(ret, 0)

        # Old unix timestamp
        ret, out, _ = run_proc(RNPK, gparam + ['1000', '--userid', 'key-ts-1000'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*sec.*1970\-01\-0.*EXPIRES 1972\-.*ssb.*1970\-01\-0.*EXPIRES 1972\-.*$')
        ret, _, _ = run_proc(RNPK, rparam + ['key-ts-1000'], 'y\n')
        self.assertEqual(ret, 0)

        # Modern timestamp
        ret, out, _ = run_proc(RNPK, gparam + ['1727777777', '--userid', 'key-ts-modern'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*sec.*2024\-10\-.*EXPIRES 2026\-.*ssb.*2024\-10\-0.*EXPIRES 2026\-.*$')
        ret, _, _ = run_proc(RNPK, rparam + ['key-ts-modern'], 'y\n')
        self.assertEqual(ret, 0)

        # Try day 31 of the 30-day month
        ret, out, err = run_proc(RNPK, gparam + ['2024-06-31', '--userid', 'key-31'])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*invalid date: 2024-06-31.*$')
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*$')
        ret, _, _ = run_proc(RNPK, rparam + ['key-31'], 'y\n')
        self.assertEqual(ret, 0)

        # Try day 32 of the 31-day month
        ret, out, err = run_proc(RNPK, gparam + ['2024-05-32', '--userid', 'key-32'])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*invalid date: 2024-05-32.*$')
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*$')
        ret, _, _ = run_proc(RNPK, rparam + ['key-32'], 'y\n')
        self.assertEqual(ret, 0)

        # Try 29 of February for non-leap year
        ret, out, err = run_proc(RNPK, gparam + ['2022-02-29', '--userid', 'key-2922'])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*invalid date: 2022-02-29.*$')
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*$')
        ret, _, _ = run_proc(RNPK, rparam + ['key-2922'], 'y\n')
        self.assertEqual(ret, 0)

        # Try 29 of February for leap year
        ret, out, err = run_proc(RNPK, gparam + ['2024-02-29', '--userid', 'key-2924'])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*invalid date.*$')
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*sec.*2024\-02\-.*EXPIRES 2026\-.*ssb.*2024\-02\-.*EXPIRES 2026\-.*$')
        ret, _, _ = run_proc(RNPK, rparam + ['key-2924'], 'y\n')
        self.assertEqual(ret, 0)

        # Try wrong month number
        ret, out, err = run_proc(RNPK, gparam + ['2022-17-29', '--userid', 'key-17m'])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*invalid date: 2022-17-29.*$')
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*$')
        ret, _, _ = run_proc(RNPK, rparam + ['key-17m'], 'y\n')
        self.assertEqual(ret, 0)

        # Try too large expiration month value
        ret, _, err = run_proc(RNPK, gparam + ['2024-02-29', '--expiration', '9999999999999m', '--userid', 'key-2924'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Invalid expiration \'9999999999999\'.*$')
        self.assertRegex(err, r'(?s)^.*Failed to set primary key expiration..*$')

        # Try too large expiration in years
        ret, _, err = run_proc(RNPK, gparam + ['2024-02-29', '--expiration', '1000y', '--userid', 'key-2924'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Expiration value exceed 32 bit.*$')
        self.assertRegex(err, r'(?s)^.*Failed to set primary key expiration..*$')

        # Try too distant date for expiration
        ret, out, err = run_proc(RNPK, gparam + ['2024-02-29', '--expiration', '3024-02-29', '--userid', 'key-2924'])
        if is64bit:
            self.assertEqual(ret, 1)
            self.assertRegex(err, r'(?s)^.*Expiration time exceeds 32-bit value.*$')
            self.assertRegex(err, r'(?s)^.*Failed to set primary key expiration..*$')
        else:
            self.assertEqual(ret, 0)
            self.assertRegex(err, r'(?s)^.*Warning: date 3024-02-29 is beyond of 32-bit time_t, so timestamp was reduced to maximum supported value.*$')
            self.assertRegex(out, r'(?s)^.*EXPIRES >=2038-01-19.*$')
            ret, _, _ = run_proc(RNPK, rparam + ['key-2924'], 'y\n')
            self.assertEqual(ret, 0)

        # Generate key back in the past
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--notty', '--password', PASSWORD, '--generate-key', '--current-time', '2015-02-02', '--userid', 'key-2015'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Generating a new key\.\.\..*sec.*2015\-02\-0.*EXPIRES 2017\-.*ssb.*2015\-02\-0.*EXPIRES 2017\-.*$')
        # List keys
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--notty', '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*pub.*2015-02-0.*EXPIRED 2017.*sub.*2015-02-0.* \[EXPIRED 2017.*$')
        self.assertNotRegex(out, r'(?s)^.*\[INVALID\].*$')
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--notty', '--current-time', '2015-02-04', '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*pub.*2015-02-0.*EXPIRES 2017.*sub.*2015-02-0.*EXPIRES 2017.*$')
        # Create workfile
        src, sig, enc = reg_workfiles('cleartext', '.txt', '.txt.sig', '.txt.enc')
        with open(src, 'w+') as f:
            f.write('Hello world')
        # Sign with key from the past
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '-u', 'key-2015', '-s', src, '--output', sig])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Failed to add signature.*$')
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '-u', 'key-2015', '--current-time', '2015-02-03', '-s', src, '--output', sig])
        self.assertEqual(ret, 0)
        # List packets
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR2, '--list-packets', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*signature creation time.*2015\).*signature expiration time.*$')
        # Verify with the expired key
        ret, out, err = run_proc(RNP, ['--homedir', RNPDIR2, '-v', sig, '--output', '-'])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*2015.*pub.*\[SC\] \[EXPIRED 2017.*$')
        self.assertRegex(out, r'(?s)^.*Hello world.*$')
        # Encrypt with the expired key
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR2, '-r', 'key-2015', '-e', src, '--output', enc])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Failed to add recipient.*$')
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR2, '-r', 'key-2015', '--current-time', '2015-02-03', '-e', src, '--output', enc])
        self.assertEqual(ret, 0)
        # Decrypt with the expired key
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '-d', enc, '--output', '-'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Hello world.*$')

    def test_wrong_passfd(self):
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--pass-fd', '999', '--userid',
                                      'test_wrong_passfd', '--generate-key', '--expert'], '22\n')
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Cannot open fd 999 for reading')
        self.assertRegex(err, r'(?s)^.*fatal: failed to initialize rnpkeys')

    def test_keystore_formats(self):
        # Use wrong keystore format
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--keystore-format', 'WRONG', '--list-keys'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Unsupported keystore format: "WRONG"')
        # Use G10 keystore format
        RNPG10 = RNPDIR + '/g10'
        #os.mkdir(RNPG10, 0o700)
        kring = shutil.copytree(data_path(KEYRING_DIR_3), RNPG10)
        ret, _, err = run_proc(RNPK, ['--homedir', kring, '--keystore-format', 'G10', '--list-keys'])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Warning: no keys were loaded from the keyring \'.*private-keys-v1.d\'')
        # Use G21 keystore format
        ret, out, _ = run_proc(RNPK, ['--homedir', kring, '--keystore-format', 'GPG21', '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*2 keys found')
        shutil.rmtree(RNPG10, ignore_errors=True)

    def test_no_twofish(self):
        if (RNP_TWOFISH):
            self.skipTest('Twofish is available')
        src, dst, dec = reg_workfiles('cleartext', '.txt', '.pgp', '.dec')
        random_text(src, 100)
        # Attempt to encrypt to twofish
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--cipher', 'twofish', '--output', dst, '-e', src])
        self.assertEqual(ret, 2)
        self.assertFalse(os.path.isfile(dst))
        self.assertRegex(err, r'(?s)^.*Unsupported encryption algorithm: twofish')
        # Symmetrically encrypt with GnuPG
        gpg_symencrypt_file(src, dst, 'TWOFISH')
        # Attempt to decrypt
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', dec, '-d', dst])
        self.assertEqual(ret, 1)
        self.assertFalse(os.path.isfile(dec))
        self.assertRegex(err, r'(?s)^.*failed to start cipher')
        # Public-key encrypt with GnuPG
        kpath = path_for_gpg(data_path(PUBRING_1))
        os.remove(dst)
        ret, _, _ = run_proc(GPG, ['--homedir', GPGHOME, '--no-default-keyring', '--batch', '--keyring', kpath, '-r', 'key0-uid0',
                                   '--trust-model', 'always', '--cipher-algo', 'TWOFISH', '--output', dst, '-e', src])
        self.assertEqual(ret, 0)
        # Attempt to decrypt
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--output', dec, '-d', dst])
        self.assertEqual(ret, 1)
        self.assertFalse(os.path.isfile(dec))
        self.assertRegex(err, r'(?s)^.*Unsupported symmetric algorithm 10')

    def test_no_idea(self):
        if (RNP_IDEA):
            self.skipTest('IDEA is available')
        src, dst, dec = reg_workfiles('cleartext', '.txt', '.pgp', '.dec')
        random_text(src, 100)
        # Attempt to encrypt to twofish
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--cipher', 'idea', '--output', dst, '-e', src])
        self.assertEqual(ret, 2)
        self.assertFalse(os.path.isfile(dst))
        self.assertRegex(err, r'(?s)^.*Unsupported encryption algorithm: idea')
        # Symmetrically encrypt with GnuPG
        gpg_symencrypt_file(src, dst, 'IDEA')
        # Attempt to decrypt
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', dec, '-d', dst])
        self.assertEqual(ret, 1)
        self.assertFalse(os.path.isfile(dec))
        self.assertRegex(err, r'(?s)^.*failed to start cipher')
        # Public-key encrypt with GnuPG
        kpath = path_for_gpg(data_path(PUBRING_1))
        os.remove(dst)
        params = ['--no-default-keyring', '--batch', '--keyring', kpath, '-r', 'key0-uid0', '--trust-model', 'always', '--cipher-algo', 'IDEA', '--output', dst, '-e', src]
        if GPG_NO_OLD:
            params.insert(1, '--allow-old-cipher-algos')
        ret, _, _ = run_proc(GPG, params)
        self.assertEqual(ret, 0)
        # Attempt to decrypt
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--output', dec, '-d', dst])
        self.assertEqual(ret, 1)
        self.assertFalse(os.path.isfile(dec))
        self.assertRegex(err, r'(?s)^.*Unsupported symmetric algorithm 1')
        # List secret key, encrypted with IDEA
        ret, out, err = run_proc(RNP, ['--homedir', RNPDIR, '--list-packets', data_path('keyrings/4/rsav3-s.asc')])
        self.assertEqual(ret, 0)
        self.assertNotRegex(out, r'(?s)^.*failed to process packet')
        self.assertRegex(out, r'(?s)^.*secret key material.*symmetric algorithm: 1 .IDEA.')
        # Import secret key - must succeed.
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('keyrings/4/rsav3-s.asc')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*sec.*7d0bc10e933404c9.*INVALID')

    def test_subkey_binding_on_uid(self):
        # Import key with deleted subkey packet (so subkey binding is attached to the uid)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('test_key_edge_cases/alice-uid-binding.pgp')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*pub.*0451409669ffde3c.*alice@rnp.*$')
        # List keys - make sure rnp doesn't attempt to validate wrong sig
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR2, '--list-keys', '--with-sigs'])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*wrong lbits.*$')
        self.assertRegex(err, r'(?s)^.*Invalid binding signature key type.*$')
        self.assertRegex(out, r'(?s)^.*sig.*alice@rnp.*.*sig.*alice@rnp.*invalid.*$')

    def test_key_locate(self):
        seckey = data_path(SECRING_1)
        pubkey = data_path(PUBRING_1)
        src, sig = reg_workfiles('cleartext', '.txt', '.sig')
        random_text(src, 1200)
        # Try non-existing key
        ret, _, err = run_proc(RNP, ['--keyfile', seckey, '-u', 'alice', '--sign', src, '--output', sig])
        self.assertEqual(ret, 1)
        self.assertRegex(err, r'(?s)^.*Cannot find key matching "alice".*')
        # Match via partial uid
        ret, _, _ = run_proc(RNP, ['--keyfile', seckey, '-u', 'key0', '--password', PASSWORD, '--sign', src, '--output', sig])
        self.assertEqual(ret, 0)
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*7bc6709b15c23a4a.*Signature\(s\) verified successfully.*')
        remove_files(sig)
        R_GOOD_SIG = r'(?s)^.*Good signature made.*2fcadf05ffa501bb.*Signature\(s\) verified successfully.*'
        # Match via keyid with hex prefix
        ret, _, _ = run_proc(RNP, ['--keyfile', seckey, '-u', '0x2fcadf05ffa501bb', '--password', PASSWORD, '--sign', src, '--output', sig])
        self.assertEqual(ret, 0)
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD_SIG)
        remove_files(sig)
        # Match via keyid with spaces/tabs
        ret, _, _ = run_proc(RNP, ['--keyfile', seckey, '-u', '0X 2FCA DF05\tFFA5\t01BB', '--password', PASSWORD, '--sign', src, '--output', sig])
        self.assertEqual(ret, 0)
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD_SIG)
        remove_files(sig)
        # Match via half of the keyid
        ret, _, _ = run_proc(RNP, ['--keyfile', seckey, '-u', 'FFA501BB', '--password', PASSWORD, '--sign', src, '--output', sig])
        self.assertEqual(ret, 0)
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD_SIG)
        remove_files(sig)
        # Match via fingerprint
        ret, _, _ = run_proc(RNP, ['--keyfile', seckey, '-u', 'be1c4ab9 51F4C2F6 b604c7f8 2FCADF05 ffa501bb', '--password', PASSWORD, '--sign', src, '--output', sig])
        self.assertEqual(ret, 0)
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD_SIG)
        remove_files(sig)
        # Match via grip
        ret, _, _ = run_proc(RNP, ['--keyfile', seckey, '-u', '0xb2a7f6c34aa2c15484783e9380671869a977a187', '--password', PASSWORD, '--sign', src, '--output', sig])
        self.assertEqual(ret, 0)
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD_SIG)
        remove_files(sig)
        # Match via regexp
        ret, _, _ = run_proc(RNP, ['--keyfile', seckey, '-u', 'key[12].uid.', '--password', PASSWORD, '--sign', src, '--output', sig])
        self.assertEqual(ret, 0)
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-v', sig])
        self.assertEqual(ret, 0)
        self.assertRegex(err, R_GOOD_SIG)

    def test_conflicting_commands(self):
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '--generate-key', '--import', '--revoke-key', '--list-keys'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Conflicting commands!*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR, '-g', '-l'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Conflicting commands!*')
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--sign', '--verify', '--decrypt', '--list-packets'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Conflicting commands!*')
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '-s', '-v'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Conflicting commands!*')

    def test_hidden_recipient(self):
        seckey = data_path(SECRING_1)
        msg1 = data_path('test_messages/message.txt.enc-hidden-1')
        msg2 = data_path('test_messages/message.txt.enc-hidden-2')
        pswd = 'password\n'
        pswds = 'password\npassword\npassword\npassword\n'
        R_MSG = r'(?s)^.*This is test message to be signed.*'
        H_MSG1 = r'(?s)^.*Warning: message has hidden recipient, but it was ignored. Use --allow-hidden to override this.*'
        H_MSG2 = r'(?s)^.*This message has hidden recipient. Will attempt to use all secret keys for decryption.*'
        # Try to decrypt message without valid key
        ret, out, err = run_proc(RNP, ['--keyfile', data_path(KEY_ALICE_SUB_SEC), '--notty', '-d', msg1], pswd)
        self.assertEqual(ret, 1)
        self.assertNotRegex(out, R_MSG)
        self.assertNotRegex(out, r'(?s)^.*Enter password for key.*')
        self.assertRegex(err, H_MSG1)
        # Try to decrypt message with first recipient hidden, it must not be asked for
        ret, out, _ = run_proc(RNP, ['--keyfile', seckey, '--notty', '-d', msg1], pswd)
        self.assertEqual(ret, 0)
        self.assertRegex(out, R_MSG)
        self.assertRegex(out, r'(?s)^.*Enter password for key 0x326EF111425D14A5 to decrypt.*')
        self.assertNotRegex(out, r'(?s)^.*Enter password.*Enter password.*')
        # Try to decrypt message with first recipient hidden, providing wrong password
        ret, out, err = run_proc(RNP, ['--keyfile', seckey, '--notty', '-d', msg1], '123\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, r'(?s)^.*Enter password for key 0x326EF111425D14A5 to decrypt.*')
        self.assertNotRegex(out, r'(?s)^.*Enter password.*Enter password.*')
        self.assertRegex(err, H_MSG1)
        # Try to decrypt message with second recipient hidden
        ret, out, _ = run_proc(RNP, ['--keyfile', seckey, '--notty', '-d', msg2], pswd)
        self.assertEqual(ret, 0)
        self.assertRegex(out, R_MSG)
        self.assertRegex(out, r'(?s)^.*Enter password for key 0x8A05B89FAD5ADED1 to decrypt.*')
        self.assertNotRegex(out, r'(?s)^.*Enter password.*Enter password.*')
        # Try to decrypt message with second recipient hidden, wrong password
        ret, out, err = run_proc(RNP, ['--keyfile', seckey, '--notty', '-d', msg2], '123\n')
        self.assertEqual(ret, 1)
        self.assertRegex(out, r'(?s)^.*Enter password for key 0x8A05B89FAD5ADED1 to decrypt.*')
        self.assertNotRegex(out, r'(?s)^.*Enter password.*Enter password.*')
        self.assertRegex(err, H_MSG1)
        # Allow hidden recipient, specifying valid password
        ret, out, err = run_proc(RNP, ['--keyfile', seckey, '--notty', '--allow-hidden', '-d', msg1], pswds)
        self.assertEqual(ret, 0)
        self.assertRegex(err, H_MSG2)
        self.assertRegex(out, R_MSG)
        self.assertRegex(out, r'(?s)^.*Enter password for key 0x1ED63EE56FADC34D.*0x8A05B89FAD5ADED1.*')
        self.assertNotRegex(out, r'(?s)^.*Enter password.*Enter password.*Enter password.*')
        # Allow hidden recipient, specifying all wrong passwords
        ret, out, err = run_proc(RNP, ['--keyfile', seckey, '--notty', '--allow-hidden', '-d', msg1], '1\n1\n1\n1\n')
        self.assertEqual(ret, 1)
        self.assertRegex(err, H_MSG2)
        self.assertRegex(out, r'(?s)^.*Enter password for key 0x1ED63EE56FADC34D.*0x8A05B89FAD5ADED1.*0x326EF111425D14A5.*')
        self.assertNotRegex(out, r'(?s)^.*Enter password.*Enter password.*Enter password.*Enter password.*')
        # Allow hidden recipient, specifying invalid password for first recipient and valid password for hidden, message 2
        ret, out, err = run_proc(RNP, ['--keyfile', seckey, '--notty', '--allow-hidden', '-d', msg2], '1\npassword\npassword\n')
        self.assertEqual(ret, 0)
        self.assertRegex(err, H_MSG2)
        self.assertRegex(out, R_MSG)
        self.assertRegex(out, r'(?s)^.*Enter password for key 0x8A05B89FAD5ADED1.*0x54505A936A4A970E.*0x326EF111425D14A5.*')
        self.assertNotRegex(out, r'(?s)^.*Enter password.*Enter password.*Enter password.*Enter password.*')
        # Allow hidden recipient, specifying invalid password for all, message 2
        ret, out, err = run_proc(RNP, ['--keyfile', seckey, '--notty', '--allow-hidden', '-d', msg2], '1\n1\n1\n1\n')
        self.assertEqual(ret, 1)
        self.assertRegex(err, H_MSG2)
        self.assertRegex(out, r'(?s)^.*Enter password for key 0x8A05B89FAD5ADED1.*0x54505A936A4A970E.*0x326EF111425D14A5.*')
        self.assertNotRegex(out, r'(?s)^.*Enter password.*Enter password.*Enter password.*Enter password.*')

    def test_allow_weak_hash(self):
        # rnpkeys, force weak hashes for key generation
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR2, '-g', '--password=', '--hash', 'MD5'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Hash algorithm \'MD5\' is cryptographically weak!.*Weak hash algorithm detected. Pass --allow-weak-hash option if you really want to use it\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR2, '-g', '--password=', '--hash', 'MD5', '--allow-weak-hash'])
        self.assertEqual(ret, 0)

        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR2, '-g', '--password=', '--hash', 'SHA1'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Hash algorithm \'SHA1\' is cryptographically weak!.*Weak hash algorithm detected. Pass --allow-weak-hash option if you really want to use it\..*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR2, '-g', '--password=', '--hash', 'SHA1', '--allow-weak-hash'])
        self.assertEqual(ret, 0)

        # check non-weak hash
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR2, '-g', '--password=', '--hash', 'SHA3-512'])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*Hash algorithm \'SHA3\-512\' is cryptographically weak!.*')
        ret, _, err = run_proc(RNPK, ['--homedir', RNPDIR2, '-g', '--password=', '--hash', 'SHA3-512', '--allow-weak-hash'])
        self.assertEqual(ret, 0)

        # rnp, force weak hashes for signature
        src, sig = reg_workfiles('cleartext', '.txt', '.sig')
        random_text(src, 120)

        ret, _, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--sign', src, '--output', sig, '--hash', 'MD5'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Hash algorithm \'MD5\' is cryptographically weak!.*Weak hash algorithm detected. Pass --allow-weak-hash option if you really want to use it\..*')
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--sign', src, '--output', sig, '--hash', 'MD5', '--allow-weak-hash'])
        self.assertEqual(ret, 0)
        remove_files(sig)

        ret, _, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--sign', src, '--output', sig, '--hash', 'SHA1'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Hash algorithm \'SHA1\' is cryptographically weak!.*Weak hash algorithm detected. Pass --allow-weak-hash option if you really want to use it\..*')
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--sign', src, '--output', sig, '--hash', 'SHA1', '--allow-weak-hash'])
        self.assertEqual(ret, 0)
        remove_files(sig)

        # check non-weak hash
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--sign', src, '--output', sig, '--hash', 'SHA3-512'])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*Hash algorithm \'SHA3\-512\' is cryptographically weak!.*')
        remove_files(sig)
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--sign', src, '--output', sig, '--hash', 'SHA3-512', '--allow-weak-hash'])
        self.assertEqual(ret, 0)
        remove_files(sig)

    def test_allow_sha1_key_sigs(self):
        src, sig = reg_workfiles('cleartext', '.txt', '.sig')
        random_text(src, 120)

        ret, out, _ = run_proc(RNPK, ['--keyfile', data_path(PUBRING_7), '--notty', '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*\[INVALID\].*$')
        ret, out, _ = run_proc(RNPK, ['--keyfile', data_path(PUBRING_7), '--notty', '--list-keys', '--allow-sha1-key-sigs'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*pub.*2024-06-03.*sub.*2024-06-03.*$')
        self.assertNotRegex(out, r'(?s)^.*\[INVALID\].*$')

        ret, _, err = run_proc(RNP, ['--keyfile', data_path(PUBRING_7), '--notty', '--password=', '-e', src, '--output', sig])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Failed to add recipient.*')
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(PUBRING_7), '--notty', '--password=', '-e', src, '--output', sig, '--allow-sha1-key-sigs'])
        self.assertEqual(ret, 0)
        remove_files(sig)

        ret, _, err = run_proc(RNP, ['--keyfile', data_path(PUBRING_7), '--notty', '--password=', '-e', src, '--output', sig, '--hash', 'SHA1'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Hash algorithm \'SHA1\' is cryptographically weak!.*Weak hash algorithm detected. Pass --allow-weak-hash option if you really want to use it\..*')
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(PUBRING_7), '--notty', '--password=', '-e', src, '--output', sig, '--hash', 'SHA1', '--allow-sha1-key-sigs'])
        self.assertEqual(ret, 0)

    def test_allow_old_ciphers(self):
        if not RNP_CAST5:
            self.skipTest("CAST5 not supported")
        ret, _, err = run_proc(RNPK, ['--cipher', 'CAST5', '--homedir', RNPDIR2, '--password', 'password', '--current-time', '2030-01-01',
                                        '--userid', 'test_user', '--generate-key'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Cipher algorithm \'CAST5\' is cryptographically weak!.*Old cipher detected. Pass --allow-old-ciphers option if you really want to use it\..*')
        ret, _, err = run_proc(RNPK, ['--cipher', 'CAST5', '--homedir', RNPDIR2, '--password', 'password', '--current-time', '2030-01-01',
                                        '--userid', 'test_user', '--generate-key', '--allow-old-ciphers'])
        self.assertEqual(ret, 0)

        src, sig = reg_workfiles('cleartext', '.txt', '.sig')
        random_text(src, 120)

        ret, _, err = run_proc(RNP, ['-c', src, '--output', sig, '--cipher', 'CAST5', '--password', 'password', '--current-time', '2030-01-01'])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Cipher algorithm \'CAST5\' is cryptographically weak!.*Old cipher detected. Pass --allow-old-ciphers option if you really want to use it\..*')
        ret, _, err = run_proc(RNP, ['-c', src, '--output', sig, '--cipher', 'CAST5', '--password', 'password', '--current-time', '2030-01-01', '--allow-old-ciphers'])
        self.assertEqual(ret, 0)

    def test_armored_detection_on_cleartext(self):
        ret, out, err = run_proc(RNP, ['--keyfile', data_path(SECRING_1), '--password', PASSWORD, '--clearsign'], 'Hello\n')
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*BEGIN PGP SIGNED MESSAGE.*$')
        self.assertRegex(out, r'(?s)^.*BEGIN PGP SIGNATURE.*$')
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(PUBRING_1), '--verify', '-'], out)
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*$')
        self.assertNotRegex(err, r'(?s)^.*Warning: missing or malformed CRC line.*$')
        self.assertNotRegex(err, r'(?s)^.*wrong armor trailer.*$')

    def test_default_v5_key(self):
        # Import v5 key
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('test_stream_key_load/v5-rsa-sec.asc')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*sec.*b856a4197113d431.*v5_rsa.*$')
        # Attempt to sign using the default key
        ret, out, err = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '--armor', '--sign'], 'Hello')
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*Default key not found.*$')
        self.assertRegex(out, r'(?s)^.*BEGIN PGP MESSAGE.*$')
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR2, '--verify'], out)
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*$')
        # Attempt to sign cleartext using the default key
        ret, out, err = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '--clearsign'], 'Hello\n')
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*Default key not found.*$')
        self.assertRegex(out, r'(?s)^.*BEGIN PGP SIGNED MESSAGE.*$')
        self.assertRegex(out, r'(?s)^.*BEGIN PGP SIGNATURE.*$')
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR2, '--verify', '-'], out)
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*$')
        self.assertNotRegex(err, r'(?s)^.*Warning: missing or malformed CRC line.*$')
        self.assertNotRegex(err, r'(?s)^.*wrong armor trailer.*$')
        # Attempt to encrypt using the default key
        ret, out, err = run_proc(RNP, ['--homedir', RNPDIR2, '--armor', '--encrypt'], 'Hello')
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, r'(?s)^.*Default key not found.*$')
        self.assertRegex(out, r'(?s)^.*BEGIN PGP MESSAGE.*$')
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '--decrypt'], out)
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*Hello.*$')

    def test_warning_source_path_prefix_cropping(self):
        ret, _, err = run_proc(RNPK, ['--keyfile', data_path(PUBRING_7), '--notty', '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*\[signature_validate\(\) [/\\]lib[/\\]crypto[/\\]signatures.cpp:[0-9]*\] Insecure hash algorithm [0-9]*, marking signature as invalid.*$')

    def test_armor_with_spaces_import(self):
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('test_stream_key_load/ecc-25519-pub.spaces.asc')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*pub.*255/EdDSA.*cc786278981b0728.*')
        self.assertRegex(err, r'(?s)^.*Import finished: 1 key processed, 1 new public keys.*')

    def test_gpg_armored(self):
        ret, out, err = run_proc(RNP, ['--homedir', RNP, '--dearmor', data_path('test_messages/message.txt.gpg-armored')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*This is test message to be signed.*')
        self.assertFalse(err)

    def test_armor_headers(self):
        ret, out, err = run_proc(RNP, ['--homedir', RNP, '--dearmor', data_path('test_messages/message.txt.armor-hdrs')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*This is test message to be signed.*')
        self.assertFalse(err)
        ret, out, err = run_proc(RNP, ['--homedir', RNP, '--dearmor', data_path('test_messages/message.txt.armor-wr-trail')])
        self.assertNotEqual(ret, 0)
        self.assertNotRegex(out, r'(?s)^.*This is test message to be signed.*')
        self.assertRegex(err, r'(?s)^.*wrong armor trailer.*')
        self.assertRegex(err, r'(?s)^.*dearmoring failed.*')

    def test_default_rsa_keygen(self):
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR2, '--generate', '--password', PASSWORD])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*sec.*3072/RSA.*\[SC\].*ssb.*3072/RSA.*\[E\].*')
        self.assertRegex(err, r'(?s)^.*Keyring directory.*is empty.*')

class Encryption(unittest.TestCase):
    '''
        Things to try later:
        - different public key algorithms
        - different hash algorithms where applicable

        TODO:
        Tests in this test case should be split into many algorithm-specific tests
        (potentially auto generated)
        Reason being - if you have a problem with BLOWFISH size 1000000, you don't want
        to wait until everything else gets
        tested before your failing BLOWFISH
    '''
    # Ciphers list to try during encryption. None will use default
    CIPHERS = [None]
    SIZES = [20, 40, 120, 600, 1000, 5000, 20000, 250000]
    # Compression parameters to try during encryption(s)
    Z = [[None, 0], ['zip'], ['zlib'], ['bzip2'], [None, 1], [None, 9]]
    # Number of test runs - each run picks next encryption algo and size, wrapping on array
    RUNS = 20

    @classmethod
    def setUpClass(cls):
        # Generate keypair in RNP
        rnp_genkey_rsa(KEY_ENCRYPT)
        # Add some other keys to the keyring
        rnp_genkey_rsa('dummy1@rnp', 1024)
        rnp_genkey_rsa('dummy2@rnp', 1024)
        gpg_import_pubring()
        gpg_import_secring()
        Encryption.CIPHERS += rnp_supported_ciphers(False)
        Encryption.CIPHERS_R = list_upto(Encryption.CIPHERS, Encryption.RUNS)
        Encryption.SIZES_R = list_upto(Encryption.SIZES, Encryption.RUNS)
        Encryption.Z_R = list_upto(Encryption.Z, Encryption.RUNS)

    @classmethod
    def tearDownClass(cls):
        clear_keyrings()

    def tearDown(self):
        clear_workfiles()

    # Encrypt cleartext file with GPG and decrypt it with RNP,
    # using different ciphers and file sizes
    def test_file_encryption__gpg_to_rnp(self):
        for size, cipher in zip(Encryption.SIZES_R, Encryption.CIPHERS_R):
            gpg_to_rnp_encryption(size, cipher)

    # Encrypt with RNP and decrypt with GPG
    def test_file_encryption__rnp_to_gpg(self):
        for size in Encryption.SIZES:
            file_encryption_rnp_to_gpg(size)

    def test_sym_encryption__gpg_to_rnp(self):
        # Encrypt cleartext with GPG and decrypt with RNP
        for size, cipher, z in zip(Encryption.SIZES_R, Encryption.CIPHERS_R, Encryption.Z_R):
            rnp_sym_encryption_gpg_to_rnp(size, cipher, z)

    def test_sym_encryption__rnp_to_gpg(self):
        # Encrypt cleartext with RNP and decrypt with GPG
        for size, cipher, z in zip(Encryption.SIZES_R, Encryption.CIPHERS_R, Encryption.Z_R):
            rnp_sym_encryption_rnp_to_gpg(size, cipher, z, 1024)

    def test_sym_encryption_s2k_iter(self):
        src, enc = reg_workfiles('cleartext', '.txt', '.gpg')
        # Generate random file of required size
        random_text(src, 20)
        def s2k_iter_run(input_iterations, expected_iterations):
            # Encrypt cleartext file with RNP
            ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--output', enc, '--password', PASSWORD, '-c', '--s2k-iterations', str(input_iterations), src])
            if ret != 0:
                raise_err('rnp encryption failed', err)
            ret, out, _ = run_proc(RNP, ['--list-packets', enc])
            self.assertEqual(ret, 0)
            self.assertRegex(out, r'(?s)^.*s2k iterations: [0-9]+ \(encoded as [0-9]+\).*')
            matches = re.findall(r'(?s)^.*s2k iterations: ([0-9]+) \(encoded as [0-9]+\).*', out)
            if int(matches[0]) != expected_iterations:
                raise_err('unexpected iterations number', matches[0])
            remove_files(enc)

        for iters in [1024, 1088, 0x3e00000]:
            s2k_iter_run(iters, iters)
        clear_workfiles()

    def test_sym_encryption_s2k_msec(self):
        src, enc = reg_workfiles('cleartext', '.txt', '.gpg')
        # Generate random file of required size
        random_text(src, 20)
        def s2k_msec_iters(msec):
            # Encrypt cleartext file with RNP
            ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--output', enc, '--password', PASSWORD, '-c', '--s2k-msec', str(msec), src])
            if ret != 0:
                raise_err('rnp encryption failed', err)
            ret, out, _ = run_proc(RNP, ['--list-packets', enc])
            self.assertEqual(ret, 0)
            self.assertRegex(out, r'(?s)^.*s2k iterations: [0-9]+ \(encoded as [0-9]+\).*')
            matches = re.findall(r'(?s)^.*s2k iterations: (\d+) \(encoded as \d+\).*', out)
            remove_files(enc)
            return int(matches[0])

        iters1msec = s2k_msec_iters(1)
        iters10msec = s2k_msec_iters(10)
        iters100msec = s2k_msec_iters(100)

        disable_test = os.getenv('DISABLE_TEST_S2K_MSEC')
        if disable_test is None:
            self.assertGreaterEqual(iters10msec, iters1msec)
            self.assertGreaterEqual(iters100msec, iters10msec)
        clear_workfiles()

    def test_sym_encryption_wrong_s2k(self):
        src, dst, enc = reg_workfiles('cleartext', '.txt', '.rnp', '.enc')
        random_text(src, 1001)
        # Wrong S2K iterations
        ret, _, err = run_proc(RNP, ['--s2k-iterations', 'WRONG_ITER', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--output', enc, '-c', src])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Wrong iterations value: WRONG_ITER.*')
        # Wrong S2K msec
        ret, _, err = run_proc(RNP, ['--s2k-msec', 'WRONG_MSEC', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--output', enc, '-c', src])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Invalid s2k msec value: WRONG_MSEC.*')
        # Overflow
        ret, _, err = run_proc(RNP, ['--s2k-iterations', '999999999999', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--output', enc, '-c', src])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Wrong iterations value: 999999999999.*')
        self.assertNotRegex(err, r'(?s)^.*std::out_of_range.*')

        ret, _, err = run_proc(RNP, ['--s2k-msec', '999999999999', '--homedir', RNPDIR, '--password', PASSWORD,
                                      '--output', enc, '-c', src])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Invalid s2k msec value: 999999999999.*')
        self.assertNotRegex(err, r'(?s)^.*std::out_of_range.*')

        remove_files(src, dst, enc)

    def test_sym_encryption__rnp_aead(self):
        if not RNP_AEAD:
            self.skipTest('AEAD is not available for RNP - skipping.')
        CIPHERS = rnp_supported_ciphers(True)
        AEADS = [None, 'eax', 'ocb']
        if not RNP_AEAD_EAX:
            AEADS.remove('eax')
        AEAD_C = list_upto(CIPHERS, Encryption.RUNS)
        AEAD_M = list_upto(AEADS, Encryption.RUNS)
        AEAD_B = list_upto([None, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16], Encryption.RUNS)
        SIZES = Encryption.SIZES_R
        random.shuffle(SIZES)

        # Encrypt and decrypt cleartext using the AEAD
        for size, cipher, aead, bits, z in zip(SIZES, AEAD_C,
                                               AEAD_M, AEAD_B, Encryption.Z_R):
            if RNP_BOTAN_OCB_AV and (aead == 'ocb') and (size > 30000):
                continue
            rnp_sym_encryption_rnp_aead(size, cipher, z, [aead, bits], GPG_AEAD)

    def test_sym_encryption_aead_def_mode(self):
        if not RNP_AEAD:
            self.skipTest('AEAD is not available for RNP - skipping.')

        src, enc = reg_workfiles('cleartext', '.txt', '.enc')
        random_text(src, 20000)
        # Check whether by default OCB is selected
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', enc, '--aead', '-z', '0', '-c', src])
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*AEAD\-encrypted data packet.*aead algorithm: 2 \(OCB\)')
        # Try to use EAX and see whether there is a warning
        remove_files(enc)
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', enc, '--aead=eax', '-z', '0', '-c', src])
        if RNP_AEAD_EAX:
            self.assertEqual(ret, 0)
            self.assertRegex(err, r'(?s)^.*rnp_op_encrypt_set_aead.*rnp.cpp.*Warning! EAX mode is deprecated and should not be used')
        else:
            self.assertEqual(ret, 1)
            self.assertRegex(err, r'(?s)^.*Invalid AEAD algorithm: EAX')

    def test_sym_encrypted__rnp_aead_botan_crash(self):
        if RNP_BOTAN_OCB_AV:
            return
        dst, = reg_workfiles('cleartext', '.txt')
        rnp_decrypt_file(data_path('test_messages/message.aead-windows-issue'), dst)
        remove_files(dst)
        rnp_decrypt_file(data_path('test_messages/message.aead-windows-issue2'), dst)
        remove_files(dst)

    def test_aead_chunk_edge_cases(self):
        if not RNP_AEAD:
            self.skipTest('AEAD is not available for RNP - skipping.')
        src, dst, enc = reg_workfiles('cleartext', '.txt', '.rnp', '.enc')
        # Cover lines from src_skip() where > 16 bytes must be skipped
        random_text(src, 1001)
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', enc, '--aead=eax', '--aead-chunk-bits', '2', '-z', '0', '-c', src])
        if RNP_AEAD_EAX:
            self.assertEqual(ret, 0)
            rnp_decrypt_file(enc, dst)
            remove_files(dst, enc)
        else:
            self.assertEqual(ret, 1)
            self.assertRegex(err, r'(?s)^.*Invalid AEAD algorithm: EAX')
        # Check non-AES OCB mode
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', enc, '--cipher', 'CAMELLIA192', '--aead=ocb', '--aead-chunk-bits', '2', '-z', '0', '-c', src])
        if RNP_AEAD_OCB_AES:
            self.assertEqual(ret, 1)
            self.assertRegex(err, r'(?s)^.*Only AES-OCB is supported by the OpenSSL backend')
        else:
            self.assertEqual(ret, 0)
            rnp_decrypt_file(enc, dst)
            remove_files(dst, enc)
        # Check default (AES) OCB
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', enc, '--aead=ocb', '--aead-chunk-bits', '2', '-z', '0', '-c', src])
        self.assertEqual(ret, 0)
        rnp_decrypt_file(enc, dst)
        remove_files(src, dst, enc)
        # Cover case with AEAD chunk start on the data end
        random_text(src, 1002)
        if RNP_AEAD_EAX:
            ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', enc, '--aead=eax', '--aead-chunk-bits', '2', '-z', '0', '-c', src])
            self.assertEqual(ret, 0)
            rnp_decrypt_file(enc, dst)
            remove_files(dst, enc)
        if RNP_AEAD_OCB:
            ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', enc, '--aead-chunk-bits', '2', '-z', '0', '-c', src])
            self.assertEqual(ret, 0)
            rnp_decrypt_file(enc, dst)
        remove_files(src, dst, enc)

    def fill_aeads(self, runs):
        aead = [None, [None]]
        if RNP_AEAD_EAX:
            aead += [['eax']]
        if RNP_AEAD_OCB:
            aead += [['ocb']]
        return list_upto(aead, runs)

    def gpg_supports(self, aead):
        if (aead == ['eax']) and not GPG_AEAD_EAX:
            return False
        if (aead == ['ocb']) and not GPG_AEAD_OCB:
            return False
        if (aead == [None]) and not GPG_AEAD_OCB:
            return False
        return True

    def test_encryption_multiple_recipients(self):
        USERIDS = ['key1@rnp', 'key2@rnp', 'key3@rnp']
        KEYPASS = ['key1pass', 'key2pass', 'key3pass']
        PASSWORDS = ['password1', 'password2', 'password3']
        # Generate multiple keys and import to GnuPG
        for uid, pswd in zip(USERIDS, KEYPASS):
            rnp_genkey_rsa(uid, 1024, pswd)

        gpg_import_pubring()
        gpg_import_secring()

        KEYPSWD = tuple((t1, t2) for t1 in range(len(USERIDS) + 1)
                        for t2 in range(len(PASSWORDS) + 1))
        KEYPSWD = list_upto(KEYPSWD, Encryption.RUNS)
        AEADS = self.fill_aeads(Encryption.RUNS)

        src, dst, dec = reg_workfiles('cleartext', '.txt', '.rnp', '.dec')
        # Generate random file of required size
        random_text(src, 65500)

        for kpswd, aead in zip(KEYPSWD, AEADS):
            keynum, pswdnum = kpswd
            if (keynum == 0) and (pswdnum == 0):
                continue
            uids = USERIDS[:keynum] if keynum else None
            pswds = PASSWORDS[:pswdnum] if pswdnum else None

            rnp_encrypt_file_ex(src, dst, uids, pswds, aead)

            # Decrypt file with each of the keys, we have different password for each key
            # For CFB mode there is ~5% probability that GnuPG will attempt to decrypt
            # message's SESK with a wrong password, see T3795 on dev.gnupg.org
            first_pass = aead is None and ((pswdnum > 1) or ((pswdnum == 1) and (keynum > 0)))
            try_gpg = self.gpg_supports(aead)
            for pswd in KEYPASS[:keynum]:
                if not first_pass and try_gpg:
                    gpg_decrypt_file(dst, dec, pswd)
                    gpg_agent_clear_cache()
                    remove_files(dec)
                rnp_decrypt_file(dst, dec, password='\n'.join([pswd] * 5))
                remove_files(dec)

            # Decrypt file with each of the passwords (with gpg only first password is checked)
            if first_pass and try_gpg:
                gpg_decrypt_file(dst, dec, PASSWORDS[0])
                gpg_agent_clear_cache()
                remove_files(dec)

            for pswd in PASSWORDS[:pswdnum]:
                if not first_pass and try_gpg:
                    gpg_decrypt_file(dst, dec, pswd)
                    gpg_agent_clear_cache()
                    remove_files(dec)
                rnp_decrypt_file(dst, dec, password='\n'.join([pswd] * 5))
                remove_files(dec)

            remove_files(dst, dec)

        clear_workfiles()

    def test_encryption_and_signing(self):
        USERIDS = ['enc-sign1@rnp', 'enc-sign2@rnp', 'enc-sign3@rnp']
        KEYPASS = ['encsign1pass', 'encsign2pass', 'encsign3pass']
        PASSWORDS = ['password1', 'password2', 'password3']
        AEAD_C = list_upto(rnp_supported_ciphers(True), Encryption.RUNS)
        # Generate multiple keys and import to GnuPG
        for uid, pswd in zip(USERIDS, KEYPASS):
            rnp_genkey_rsa(uid, 1024, pswd)

        gpg_import_pubring()
        gpg_import_secring()

        SIGNERS = list_upto(range(1, len(USERIDS) + 1), Encryption.RUNS)
        KEYPSWD = tuple((t1, t2) for t1 in range(1, len(USERIDS) + 1)
                        for t2 in range(len(PASSWORDS) + 1))
        KEYPSWD = list_upto(KEYPSWD, Encryption.RUNS)
        AEADS = self.fill_aeads(Encryption.RUNS)
        ZS = list_upto([None, [None, 0]], Encryption.RUNS)

        src, dst, dec = reg_workfiles('cleartext', '.txt', '.rnp', '.dec')
        # Generate random file of required size
        random_text(src, 65500)

        for i in range(0, Encryption.RUNS):
            signers = USERIDS[:SIGNERS[i]]
            signpswd = KEYPASS[:SIGNERS[i]]
            keynum, pswdnum = KEYPSWD[i]
            recipients = USERIDS[:keynum]
            passwords = PASSWORDS[:pswdnum]
            aead = AEADS[i]
            z = ZS[i]
            cipher = AEAD_C[i]
            first_pass = aead is None and ((pswdnum > 1) or ((pswdnum == 1) and (keynum > 0)))
            try_gpg = self.gpg_supports(aead)

            rnp_encrypt_and_sign_file(src, dst, recipients, passwords, signers,
                                      signpswd, aead, cipher, z)
            # Decrypt file with each of the keys, we have different password for each key
            for pswd in KEYPASS[:keynum]:
                if not first_pass and try_gpg:
                    gpg_decrypt_file(dst, dec, pswd)
                    gpg_agent_clear_cache()
                    remove_files(dec)
                rnp_decrypt_file(dst, dec, password='\n'.join([pswd] * 5))
                remove_files(dec)

            # GPG decrypts only with first password, see T3795
            if first_pass and try_gpg:
                gpg_decrypt_file(dst, dec, PASSWORDS[0])
                gpg_agent_clear_cache()
                remove_files(dec)

            # Decrypt file with each of the passwords
            for pswd in PASSWORDS[:pswdnum]:
                if not first_pass and try_gpg:
                    gpg_decrypt_file(dst, dec, pswd)
                    gpg_agent_clear_cache()
                    remove_files(dec)
                rnp_decrypt_file(dst, dec, password='\n'.join([pswd] * 5))
                remove_files(dec)

            remove_files(dst, dec)


    def verify_pqc_algo_ui_nb_to_algo_ui_str(self, stdout: str, algo_ui_exp_strs) -> None:
        stdout_lines = stdout.split('\n')
        for expected_line in algo_ui_exp_strs:
            found_this_entry : bool = False
            for line in stdout_lines:
                # compare ignore whitespaces and tabs:
                re_patt_for_algo = r'[^\t ]'
                char_list_expected = [c for c in expected_line if re.match(re_patt_for_algo, c)]
                char_list_actual = [c for c in line if re.match(re_patt_for_algo, c)]
                if char_list_expected == char_list_actual:
                    found_this_entry = True
                    break

            if not found_this_entry:
                raise RuntimeError("did not match the expected UI choice for algorithm: " + expected_line)

    def test_encryption_and_signing_pqc(self):
        if not RNP_PQC:
            return
        RNPDIR_PQC = RNPDIR + 'PQC'
        os.mkdir(RNPDIR_PQC, 0o700)
        algo_ui_exp_strs = [ "(24) Ed25519Legacy + Curve25519Legacy + (ML-KEM-768 + X25519)",
                             "(25) (ML-DSA-65 + Ed25519) + (ML-KEM-768 + X25519)",
                             "(27) (ML-DSA-65 + ECDSA-NIST-P-256) + (ML-KEM-768 + ECDH-NIST-P-256)",
                             "(28) (ML-DSA-87 + ECDSA-NIST-P-384) + (ML-KEM-1024 + ECDH-NIST-P-384)",
                             "(29) (ML-DSA-65 + ECDSA-brainpoolP256r1) + (ML-KEM-768 + ECDH-brainpoolP256r1)",
                             "(30) (ML-DSA-87 + ECDSA-brainpoolP384r1) + (ML-KEM-1024 + ECDH-brainpoolP384r1)",
                             "(31) SLH-DSA-SHA2 + MLKEM-ECDH Composite",
                             "(32) SLH-DSA-SHAKE + MLKEM-ECDH Composite",
                               ]
        USERIDS = ['enc-sign25@rnp', 'enc-sign27@rnp', 'enc-sign28@rnp', 'enc-sign29@rnp', 'enc-sign30@rnp','enc-sign32a@rnp','enc-sign32b@rnp','enc-sign32c@rnp','enc-sign24-v4-key@rnp']

        # '24' in the below array creates a v4 primary signature key with a v4 pqc subkey without a Features Subpacket. This way we test PQC encryption to a v4 subkey. RNP prefers the PQC subkey in case of a certificate having a PQC and a
        # non-PQC subkey.
        ALGO       = [25,   27,   28,   29,   30,   32, 32, 32, 24, ]
        ALGO_PARAM = [None, None, None, None, None, 1,  2,  6,  None,  ]
        aead_list = []
        passwds = [ ]
        for x in range(len(ALGO)): passwds.append('testpw' if x % 1 == 0 else '')
        for x in range(len(ALGO)): aead_list.append(None if x % 3 == 0 else ('ocb' if x % 3 == 1 else 'eax' ))
        if any(len(USERIDS) != len(x) for x in [ALGO, ALGO_PARAM]):
            raise  RuntimeError("test_encryption_and_signing_pqc: internal error: lengths of test data arrays matching")
        # Generate multiple keys and import to GnuPG
        verified_algo_nums = False
        for uid, algo, param, passwd in zip(USERIDS, ALGO, ALGO_PARAM, passwds):
            stdout = rnp_genkey_pqc(uid, algo, RNPDIR_PQC, param, passwd)
            if not verified_algo_nums:
                self.verify_pqc_algo_ui_nb_to_algo_ui_str(stdout, algo_ui_exp_strs)
                verified_algo_nums = True

        src, dst, dec = reg_workfiles('cleartext', '.txt', '.rnp', '.dec')
        # Generate random file of required size
        random_text(src, 65500)

        for i in range(0, len(USERIDS)):
            signers = [USERIDS[i]]
            #signpswd = KEYPASS[:SIGNERS[i]]
            #keynum, pswdnum = KEYPSWD[i]
            recipients = [USERIDS[i]]
            passwords = [] # SKESK for v6 not yet supported
            signerpws = [passwds[i]]

            rnp_encrypt_and_sign_file(src, dst, recipients, passwords, signers,
                                      signerpws, aead=[aead_list[i]], homedir=RNPDIR_PQC)
            # Decrypt file with each of the keys, we have different password for each key
            rnp_decrypt_file(dst, dec, password=passwds[i], homedir=RNPDIR_PQC)
            remove_files(dst, dec)

        clear_workfiles()
        shutil.rmtree(RNPDIR_PQC, ignore_errors=True)


    def test_encryption_weird_userids_special_1(self):
        uid = WEIRD_USERID_SPECIAL_CHARS
        pswd = 'encSpecial1Pass'
        rnp_genkey_rsa(uid, 1024, pswd)
        # Encrypt
        src = data_path(MSG_TXT)
        dst, dec = reg_workfiles('weird_userids_special_1', '.rnp', '.dec')
        rnp_encrypt_file_ex(src, dst, [uid], None, None)
        # Decrypt
        rnp_decrypt_file(dst, dec, password=pswd)
        compare_files(src, dec, RNP_DATA_DIFFERS)
        clear_workfiles()

    def test_encryption_weird_userids_special_2(self):
        USERIDS = [WEIRD_USERID_SPACE, WEIRD_USERID_QUOTE, WEIRD_USERID_SPACE_AND_QUOTE, WEIRD_USERID_QUOTE_AND_SPACE]
        KEYPASS = ['encSpecial2Pass1', 'encSpecial2Pass2', 'encSpecial2Pass3', 'encSpecial2Pass4']
        # Generate multiple keys
        for uid, pswd in zip(USERIDS, KEYPASS):
            rnp_genkey_rsa(uid, 1024, pswd)
        # Encrypt to all recipients
        src = data_path(MSG_TXT)
        dst, dec = reg_workfiles('weird_userids_special_2', '.rnp', '.dec')
        rnp_encrypt_file_ex(src, dst, list(map(lambda uid: uid, USERIDS)), None, None)
        # Decrypt file with each of the passwords
        for pswd in KEYPASS:
            multiple_pass_attempts = (pswd + '\n') * len(KEYPASS)
            rnp_decrypt_file(dst, dec, password=multiple_pass_attempts)
            compare_files(src, dec, RNP_DATA_DIFFERS)
            remove_files(dec)
        # Cleanup
        clear_workfiles()

    def test_encryption_weird_userids_unicode(self):
        USERIDS_1 = [
            WEIRD_USERID_UNICODE_1, WEIRD_USERID_UNICODE_2]
        USERIDS_2 = [
            WEIRD_USERID_UNICODE_1, WEIRD_USERID_UNICODE_2]
        # The idea is to generate keys with USERIDS_1 and encrypt with USERIDS_2
        # (that differ only in case)
        # But currently Unicode case-insensitive search is not working,
        # so we're encrypting with exactly the same recipient
        KEYPASS = ['encUnicodePass1', 'encUnicodePass2']
        # Generate multiple keys
        for uid, pswd in zip(USERIDS_1, KEYPASS):
            rnp_genkey_rsa(uid, 1024, pswd)
        # Encrypt to all recipients
        src = data_path('test_messages') + '/message.txt'
        dst, dec = reg_workfiles('weird_unicode', '.rnp', '.dec')
        rnp_encrypt_file_ex(src, dst, list(map(lambda uid: uid, USERIDS_2)), None, None)
        # Decrypt file with each of the passwords
        for pswd in KEYPASS:
            multiple_pass_attempts = (pswd + '\n') * len(KEYPASS)
            rnp_decrypt_file(dst, dec, password=multiple_pass_attempts)
            compare_files(src, dec, RNP_DATA_DIFFERS)
            remove_files(dec)
        # Cleanup
        clear_workfiles()

    def test_encryption_x25519(self):
        # Make sure that we support import and decryption using both tweaked and non-tweaked keys
        KEY_IMPORT = r'(?s)^.*' \
        r'sec.*255/EdDSA.*3176fc1486aa2528.*' \
        r'uid.*eddsa-25519-non-tweaked.*' \
        r'ssb.*255/ECDH.*950ee0cd34613dba.*$'
        BITS_MSG = r'(?s)^.*Warning: bits of 25519 secret key are not tweaked.*$'

        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path(KEY_25519_NOTWEAK_SEC)])
        self.assertEqual(ret, 0)
        self.assertRegex(out, KEY_IMPORT)
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '-d', data_path(MSG_ES_25519)])
        self.assertEqual(ret, 0)
        self.assertRegex(err, BITS_MSG)
        self.assertRegex(err, r'(?s)^.*Signature\(s\) verified successfully.*$')
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', 'eddsa-25519-non-tweaked', '--force'])
        self.assertEqual(ret, 0)
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--import', data_path('test_key_edge_cases/key-25519-tweaked-sec.asc')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, KEY_IMPORT)
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '-d', data_path(MSG_ES_25519)])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, BITS_MSG)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--remove-key', 'eddsa-25519-non-tweaked', '--force'])
        self.assertEqual(ret, 0)
        # Due to issue in GnuPG it reports successful import of non-tweaked secret key in batch mode
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import', data_path(KEY_25519_NOTWEAK_SEC)])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '-d', data_path(MSG_ES_25519)])
        self.assertNotEqual(ret, 0)
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--yes', '--delete-secret-key', 'dde0ee539c017d2bd3f604a53176fc1486aa2528'])
        self.assertEqual(ret, 0)
        # Make sure GPG imports tweaked key and successfully decrypts message
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import', data_path('test_key_edge_cases/key-25519-tweaked-sec.asc')])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '-d', data_path(MSG_ES_25519)])
        self.assertEqual(ret, 0)
        # Generate
        pipe = pswd_pipe(PASSWORD)
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--pass-fd', str(pipe), '--userid',
                                      'eddsa_25519', '--generate-key', '--expert'], '22\n')
        os.close(pipe)
        self.assertEqual(ret, 0)
        # Export
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR, '--export', '--secret', 'eddsa_25519'])
        self.assertEqual(ret, 0)
        # Import key with GPG
        ret, out, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--import'], out)
        self.assertEqual(ret, 0)
        src, dst, dec = reg_workfiles('cleartext', '.txt', '.rnp', '.dec')
        # Generate random file of required size
        random_text(src, 1000)
        # Encrypt and sign with RNP
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR, '-es', '-r', 'eddsa_25519', '-u',
                                     'eddsa_25519', '--password', PASSWORD, src, '--output', dst, '--armor'])
        # Decrypt and verify with RNP
        rnp_decrypt_file(dst, dec, password='password')
        self.assertEqual(file_text(src), file_text(dec))
        remove_files(dec)
        # Decrypt and verify with GPG
        gpg_decrypt_file(dst, dec, 'password')
        self.assertEqual(file_text(src), file_text(dec))
        remove_files(dst, dec)
        # Encrypt and sign with GnuPG
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, '--always-trust', '-r', 'eddsa_25519',
                             '-u', 'eddsa_25519', '--output', dst, '-es', src])
        self.assertEqual(ret, 0)
        # Decrypt and verify with RNP
        rnp_decrypt_file(dst, dec, password='password')
        self.assertEqual(file_text(src), file_text(dec))
        # Encrypt/decrypt using the p256 key, making sure message is not displayed
        key = data_path('test_stream_key_load/ecc-p256-sec.asc')
        remove_files(dst, dec)
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-es', '-r', 'ecc-p256', '-u', 'ecc-p256', '--password', PASSWORD, src, '--output', dst])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, BITS_MSG)
        ret, _, err = run_proc(RNP, ['--keyfile', key, '-d', '--password', PASSWORD, dst, '--output', dec])
        self.assertEqual(ret, 0)
        self.assertNotRegex(err, BITS_MSG)
        # Cleanup
        clear_workfiles()

    def test_encryption_aead_defs(self):
        if not RNP_AEAD or not RNP_BRAINPOOL:
            self.skipTest('AEAD and/or Brainpool are not supported')
        # Encrypt with RNP
        pubkey = data_path(KEY_ALICE_SUB_PUB)
        src, enc, dec = reg_workfiles('cleartext', '.txt', '.enc', '.dec')
        random_text(src, 120000)
        ret, _, _ = run_proc(RNP, ['--keyfile', pubkey, '-z', '0', '-r', 'alice', '--aead', '-e', src, '--output', enc])
        self.assertEqual(ret, 0)
        # List packets
        ret, out, _ = run_proc(RNP, ['--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*tag 20, partial len.*AEAD-encrypted data packet.*version: 1.*AES-256.*OCB.*chunk size: 12.*')
        # Attempt to encrypt with too high AEAD bits value
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-r', 'alice', '--aead', '--aead-chunk-bits', '17', '-e', src, '--output', enc])
        self.assertEqual(ret, 2)
        self.assertRegex(err, r'(?s)^.*Wrong argument value 17 for aead-chunk-bits, must be 0..16.*')
        # Attempt to encrypt with wrong AEAD bits value
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-r', 'alice', '--aead', '--aead-chunk-bits', 'banana', '-e', src, '--output', enc])
        self.assertEqual(ret, 2)
        self.assertRegex(err, r'(?s)^.*Wrong argument value banana for aead-chunk-bits, must be 0..16.*')
        # Attempt to encrypt with another wrong AEAD bits value
        ret, _, err = run_proc(RNP, ['--keyfile', pubkey, '-r', 'alice', '--aead', '--aead-chunk-bits', '5banana', '-e', src, '--output', enc])
        self.assertEqual(ret, 2)
        self.assertRegex(err, r'(?s)^.*Wrong argument value 5banana for aead-chunk-bits, must be 0..16.*')
        clear_workfiles()

    def test_encryption_no_wrap(self):
        src, sig, enc, dec = reg_workfiles('cleartext', '.txt', '.sig', '.enc', '.dec')
        random_text(src, 2000)
        # Sign with GnuPG
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', PASSWORD, '-u', KEY_ENCRYPT, '--output', sig, '-s', src])
        # Additionally encrypt with RNP
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR, '-r', 'dummy1@rnp', '--no-wrap', '-e', sig, '--output', enc])
        self.assertEqual(ret, 0)
        # List packets
        ret, out, err = run_proc(GPG, ['--batch', '--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', PASSWORD, '--list-packets', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*gpg: encrypted with .*dummy1@rnp.*')
        self.assertRegex(out, r'(?s)^.*:pubkey enc packet: version 3.*:encrypted data packet:.*mdc_method: 2.*' \
                              r':compressed packet.*:onepass_sig packet:.*:literal data packet.*:signature packet.*')
        # Decrypt with GnuPG
        ret, _, err = run_proc(GPG, ['--batch', '--homedir', GPGHOME, GPG_LOOPBACK, '--passphrase', PASSWORD, '--output', dec, '-d', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*gpg: encrypted with .*dummy1@rnp.*gpg: Good signature from "encryption@rnp".*')
        self.assertEqual(file_text(dec), file_text(src))
        remove_files(dec)
        # Decrypt with RNP
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--output', dec, '-d', enc])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature.*uid\s+encryption@rnp.*Signature\(s\) verified successfully.*')
        self.assertEqual(file_text(dec), file_text(src))
        clear_workfiles()

    def test_aead_eax_botan35_decryption(self):
        # Artifact which was obtained from tests used EAX + Twofish
        if not RNP_AEAD_EAX or not RNP_TWOFISH:
            self.skipTest('AEAD-EAX is not supported')
        # See issue #2245 for the details
        [dec] = reg_workfiles('cleartext', '.txt')
        EAXSRC = data_path('test_messages/cleartext.rnp-aead-eax')
        # Decrypt and verify AEAD-EAX encrypted message by RNP
        ret, _, _ = run_proc(RNP, ['--keyfile', data_path('test_messages/seckey-aead-eax.gpg'), '--password', 'encsign1pass', '-d', EAXSRC, '--output', dec])
        self.assertEqual(ret, 0)
        remove_files(dec)
        # Decrypt it using the password
        ret, _, _ = run_proc(RNP, ['--keyfile', data_path('test_messages/pubkey-aead-eax.gpg'), '--password', 'password1', '-d', EAXSRC, '--output', dec])
        self.assertEqual(ret, 0)
        clear_workfiles()

    def test_sm2_encryption_signing(self):
        if not RNP_SM2:
            self.skipTest('SM2 is not supported or disabled')
        RNPDIR2 = RNPDIR + '2'
        os.mkdir(RNPDIR2, 0o700)
        # Import public key
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('test_stream_key_load/sm2-pub.asc')])
        self.assertEqual(ret, 0)
        # Check listing
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--list-keys'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)2 keys found.*pub.*256/SM2.*3a143c1695ae14c9.*sm2\-key.*sub.*256/SM2.*75ca025d13c1c512.*')
        # Validate signature
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR2, '-v', data_path('test_messages/message.txt.signed-sm2')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)Good signature made.*using SM2 key 3a143c1695ae14c9.*')
        # Import secret key
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('test_stream_key_load/sm2-sec.asc')])
        self.assertEqual(ret, 0)
        # Check listing
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--list-keys', '--secret'])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)2 keys found.*sec.*256/SM2.*3a143c1695ae14c9.*sm2\-key.*ssb.*256/SM2.*75ca025d13c1c512.*')
        # Decrypt encrypted file
        ret, out, _ = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '-d', data_path('test_messages/message.txt.enc-sm2')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)This is test message to be.*')
        # Decrypt and verify file
        ret, out, err = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '-d', data_path('test_messages/message.txt.enc-signed-sm2')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)This is test message to be.*')
        self.assertRegex(err, r'(?s)Good signature made.*using SM2 key 3a143c1695ae14c9.*')
        shutil.rmtree(RNPDIR2, ignore_errors=True)        

    def test_decrypt_signonly_key(self):
        ret, _, err = run_proc(RNP, ['--keyfile', data_path('test_messages/key-rsas-rsae.asc'), '--decrypt', data_path('test_messages/message-encrypted-rsas.txt.pgp')])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*failed to obtain decrypting key or password.*')

    def test_encryption_cek(self):
        RNPDIR2 = RNPDIR + '2'
        os.mkdir(RNPDIR2, 0o700)
        GPGHOME2 = GPGHOME + '2'
        GPGDIR2 = GPGDIR + '2'
        os.mkdir(GPGDIR2, 0o700)
        # Generate default key
        ret, _, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--generate', '--userid', 'test_cek', '--password', PASSWORD])
        self.assertEqual(ret, 0)
        # Import key by GnuPG
        ret, _, _ = run_proc(GPG, ['--batch', '--homedir', GPGHOME2, GPG_LOOPBACK, '--passphrase', PASSWORD, '--import', RNPDIR2 + '/secring.gpg'])
        # Encrypt some data
        src, enc = reg_workfiles('cleartext', '.txt', '.enc')
        random_text(src, 2000)
        pattern = re.compile(r'(?s)gpg: encrypted with.*test_cek.*session key: \'[\d]+:([0-9A-F]{64})\'.*')
        keys = set()

        for _ in range(10):
            ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR2, '-r', 'test_cek', '-e', src, '--overwrite', '--output', enc])
            self.assertEqual(ret, 0)
            ret, _, err = run_proc(GPG, ['--batch', '--homedir', GPGHOME2, '--show-session-key', GPG_LOOPBACK, '--passphrase', PASSWORD, '--output', '/dev/null', '-d', enc])
            self.assertEqual(ret, 0)
            m = pattern.match(err)
            self.assertTrue(m)
            key = m.group(1)
            self.assertNotIn(key, keys)
            keys.add(key)
        
        shutil.rmtree(RNPDIR2, ignore_errors=True)
        shutil.rmtree(GPGDIR2, ignore_errors=True)

class Compression(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Compression is currently implemented only for encrypted messages
        rnp_genkey_rsa(KEY_ENCRYPT)
        rnp_genkey_rsa(KEY_SIGN_GPG)
        gpg_import_pubring()
        gpg_import_secring()

    @classmethod
    def tearDownClass(cls):
        clear_keyrings()

    def tearDown(self):
        clear_workfiles()

    def test_rnp_compression(self):
        runs = 30
        levels = list_upto([None, 0, 2, 4, 6, 9], runs)
        algosrnp = list_upto([None, 'zip', 'zlib', 'bzip2'], runs)
        sizes = list_upto([20, 1000, 5000, 15000, 250000], runs)

        for level, algo, size in zip(levels, algosrnp, sizes):
            z = [algo, level]
            gpg_to_rnp_encryption(size, None, z)
            file_encryption_rnp_to_gpg(size, z)
            rnp_signing_gpg_to_rnp(size, z)

    def test_rnp_compression_corner_cases(self):
        shutil.rmtree(RNPDIR)
        kring = shutil.copytree(data_path(KEYRING_DIR_1), RNPDIR)
        gpg_import_pubring()
        gpg_import_secring()

        src = data_path('test_compression/cleartext-z-bz.txt')
        algosrnp = [None, 'zip', 'zlib', 'bzip2']

        for algo in algosrnp:
            z = [algo, None]
            dst, enc = reg_workfiles('cleartext', '.gpg', '.rnp')
            # Encrypt cleartext file with RNP
            rnp_encrypt_file_ex(src, enc, [], None, None, None, z, False)

            # Decrypt encrypted file with GPG
            gpg_decrypt_file(enc, dst, PASSWORD)
            compare_files(src, dst, GPG_DATA_DIFFERS)
            remove_files(dst)
            # Decrypt encrypted file with RNP
            rnp_decrypt_file(enc, dst)
            compare_files(src, dst, RNP_DATA_DIFFERS)
            remove_files(enc, dst)
            clear_workfiles()

class SignDefault(unittest.TestCase):
    '''
        Things to try later:
        - different public key algorithms
        - different hash algorithms where applicable
        - cleartext signing/verification
        - detached signing/verification
    '''
    # Message sizes to be tested
    SIZES = [20, 1000, 5000, 20000, 150000, 1000000]

    @classmethod
    def setUpClass(cls):
        # Generate keypair in RNP
        rnp_genkey_rsa(KEY_SIGN_RNP)
        rnp_genkey_rsa(KEY_SIGN_GPG)
        gpg_import_pubring()
        gpg_import_secring()

    @classmethod
    def tearDownClass(cls):
        clear_keyrings()

    def setUp(self):
        os.mkdir(RNPDIR2, 0o700)

    def tearDown(self):
        clear_workfiles()
        shutil.rmtree(RNPDIR2, ignore_errors=True)

    # TODO: This script should generate one test case per message size.
    #       Not sure how to do it yet
    def test_rnp_to_gpg_default_key(self):
        for size in Sign.SIZES:
            rnp_signing_rnp_to_gpg(size)
            rnp_detached_signing_rnp_to_gpg(size)
            rnp_cleartext_signing_rnp_to_gpg(size)

    def test_gpg_to_rnp_default_key(self):
        for size in Sign.SIZES:
            rnp_signing_gpg_to_rnp(size)
            rnp_detached_signing_gpg_to_rnp(size)
            rnp_detached_signing_gpg_to_rnp(size, True)
            rnp_cleartext_signing_gpg_to_rnp(size)

    def test_rnp_multiple_signers(self):
        USERIDS = ['sign1@rnp', 'sign2@rnp', 'sign3@rnp']
        KEYPASS = ['sign1pass', 'sign2pass', 'sign3pass']

        # Generate multiple keys and import to GnuPG
        for uid, pswd in zip(USERIDS, KEYPASS):
            rnp_genkey_rsa(uid, 1024, pswd)

        gpg_import_pubring()
        gpg_import_secring()

        src, dst, sig, ver = reg_workfiles('cleartext', '.txt', '.rnp', EXT_SIG, '.ver')
        # Generate random file of required size
        random_text(src, 128000)

        for keynum in range(1, len(USERIDS) + 1):
            # Normal signing
            rnp_sign_file(src, dst, USERIDS[:keynum], KEYPASS[:keynum])
            gpg_verify_file(dst, ver)
            remove_files(ver)
            rnp_verify_file(dst, ver)
            remove_files(dst, ver)

            # Detached signing
            rnp_sign_detached(src, USERIDS[:keynum], KEYPASS[:keynum])
            gpg_verify_detached(src, sig)
            rnp_verify_detached(sig)
            remove_files(sig)

            # Cleartext signing
            rnp_sign_cleartext(src, dst, USERIDS[:keynum], KEYPASS[:keynum])
            gpg_verify_cleartext(dst)
            rnp_verify_cleartext(dst)
            remove_files(dst)

        clear_workfiles()

    def test_sign_weird_userids(self):
        USERIDS = [WEIRD_USERID_SPECIAL_CHARS, WEIRD_USERID_SPACE, WEIRD_USERID_QUOTE,
            WEIRD_USERID_SPACE_AND_QUOTE, WEIRD_USERID_QUOTE_AND_SPACE,
            WEIRD_USERID_UNICODE_1, WEIRD_USERID_UNICODE_2]
        KEYPASS = ['signUnicodePass1', 'signUnicodePass2', 'signUnicodePass3', 'signUnicodePass4',
            'signUnicodePass5', 'signUnicodePass6', 'signUnicodePass7']

        # Generate multiple keys
        for uid, pswd in zip(USERIDS, KEYPASS):
            rnp_genkey_rsa(uid, 1024, pswd)

        gpg_import_pubring()
        gpg_import_secring()

        src, dst, sig, ver = reg_workfiles('cleartext', '.txt', '.rnp', EXT_SIG, '.ver')
        # Generate random file of required size
        random_text(src, 128000)

        for keynum in range(1, len(USERIDS) + 1):
            # Normal signing
            rnp_sign_file(src, dst, USERIDS[:keynum], KEYPASS[:keynum])
            gpg_verify_file(dst, ver)
            remove_files(ver)
            rnp_verify_file(dst, ver)
            remove_files(dst, ver)

            # Detached signing
            rnp_sign_detached(src, USERIDS[:keynum], KEYPASS[:keynum])
            gpg_verify_detached(src, sig)
            rnp_verify_detached(sig)
            remove_files(sig)

            # Cleartext signing
            rnp_sign_cleartext(src, dst, USERIDS[:keynum], KEYPASS[:keynum])
            gpg_verify_cleartext(dst)
            rnp_verify_cleartext(dst)
            remove_files(dst)

        clear_workfiles()

    def test_verify_bad_sig_class(self):
        ret, _, err = run_proc(RNP, ['--keyfile', data_path(KEY_ALICE_SEC), '--verify', data_path('test_messages/message.txt.signed-class19')])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Invalid document signature type: 19.*')
        self.assertNotRegex(err, r'(?s)^.*Good signature.*')
        self.assertRegex(err, r'(?s)^.*BAD signature.*Signature verification failure: 1 invalid signature')

    def test_dsa4096_key(self):
        src, dst, ver = reg_workfiles('cleartext', '.txt', '.rnp', '.ver')
        # Generate random file of required size
        random_text(src, 1000)

        # Make sure we can import a public key
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('test_stream_key_load/dsa4096-eg4096.pub.asc')])
        self.assertEqual(ret, 0)
        if RNP_BACKEND == 'botan':
            self.assertRegex(out, r'(?s)^.*pub.*4096/DSA.*7146fc248bf7c656.*SC.*sub.*4096/ElGamal.*0e6f2feced5d0e7f.*')
            self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, 2 new public keys.*')
        else:
            self.assertRegex(out, r'(?s)^.*pub.*4096/DSA.*7146fc248bf7c656.*INVALID.*sub.*4096/ElGamal.*0e6f2feced5d0e7f.*')
            self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, 2 new public keys.*')
            return

        # Make sure we can verify signature
        ret, _, err = run_proc(RNP, ['--homedir', RNPDIR2, '--verify', data_path('test_messages/message.txt.signed-dsa4096')])
        self.assertEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*Good signature made.*4096/DSA.*7146fc248bf7c656.*')
        self.assertRegex(err, r'(?s)^.*Signature\(s\) verified successfully.*')
        # Make sure we can import a secret key
        ret, out, err = run_proc(RNPK, ['--homedir', RNPDIR2, '--import', data_path('test_stream_key_load/dsa4096-eg4096.sec.asc')])
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*sec.*4096/DSA.*7146fc248bf7c656.*SC.*ssb.*4096/ElGamal.*0e6f2feced5d0e7f.*')
        self.assertRegex(err, r'(?s)^.*Import finished: 2 keys processed, .*2 new secret keys.*')
        # Make sure we can sign with it
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR2, '--password', PASSWORD, '-u', 'dsa4096', '--sign', src, '--output', dst])
        self.assertEqual(ret, 0)
        ret, _, _ = run_proc(RNP, ['--homedir', RNPDIR2, '--verify', dst, '--output', ver])
        self.assertEqual(ret, 0)
        # Make sure we can add subkey
        ret, out, _ = run_proc(RNPK, ['--homedir', RNPDIR2, '--password', PASSWORD, '--edit-key', 'dsa4096', '--add-subkey'], 'y\n')
        self.assertEqual(ret, 0)
        self.assertRegex(out, r'(?s)^.*ssb.*3072/RSA.*EXPIRES.*')

    def test_verify_enconly_key(self):
        ret, _, err = run_proc(RNP, ['--keyfile', data_path('test_messages/key-rsas-rsae.asc'), '--verify', data_path('test_messages/message-signed-rsae.txt.pgp')])
        self.assertNotEqual(ret, 0)
        self.assertRegex(err, r'(?s)^.*key is not usable for verification.*')
        self.assertNotRegex(err, r'(?s)^.*Good signature.*')
        self.assertRegex(err, r'(?s)^.*BAD signature.*Signature verification failure: 1 invalid signature')

class Encrypt(unittest.TestCase, TestIdMixin, KeyLocationChooserMixin):
    def _encrypt_decrypt(self, e1, e2, failenc = False, faildec = False):
        keyfile, src, enc_out, dec_out = reg_workfiles(self.test_id, '.gpg',
                                                         '.in', '.enc', '.dec')
        random_text(src, 0x1337)

        if not self.operation_key_location and not self.operation_key_gencmd:
            raise RuntimeError("key not found")

        if self.operation_key_location:
            self.assertTrue(e1.import_key(self.operation_key_location[0]))
            self.assertTrue(e1.import_key(self.operation_key_location[1], True))
        else:
            self.assertTrue(e1.generate_key_batch(self.operation_key_gencmd))

        self.assertTrue(e1.export_key(keyfile, False))
        self.assertTrue(e2.import_key(keyfile))
        self.assertEqual(e2.encrypt(e1.userid, enc_out, src), not failenc)
        self.assertEqual(e1.decrypt(dec_out, enc_out), not faildec)
        clear_workfiles()

    def setUp(self):
        KeyLocationChooserMixin.__init__(self)
        self.rnp = Rnp(RNPDIR, RNP, RNPK)
        self.gpg = GnuPG(GPGHOME, GPG)
        self.rnp.password = self.gpg.password = PASSWORD
        self.rnp.userid = self.gpg.userid = self.test_id + AT_EXAMPLE

    @classmethod
    def tearDownClass(cls):
        clear_keyrings()

class EncryptElgamal(Encrypt):

    GPG_GENERATE_DSA_ELGAMAL_PATTERN = """
        Key-Type: dsa
        Key-Length: {0}
        Key-Usage: sign
        Subkey-Type: ELG-E
        Subkey-Length: {1}
        Subkey-Usage: encrypt
        Name-Real: Test Testovich
        Expire-Date: 1y
        Preferences: aes256 sha256 sha384 sha512 sha1 zlib
        Name-Email: {2}
        """

    RNP_GENERATE_DSA_ELGAMAL_PATTERN = "16\n{0}\n"

    @staticmethod
    def key_pfx(sign_key_size, enc_key_size):
        return "GnuPG_dsa_elgamal_%d_%d" % (sign_key_size, enc_key_size)

    def do_test_encrypt(self, sign_key_size, enc_key_size):
        pfx = EncryptElgamal.key_pfx(sign_key_size, enc_key_size)
        self.operation_key_location = tuple((key_path(pfx, False), key_path(pfx, True)))
        self.rnp.userid = self.gpg.userid = pfx + AT_EXAMPLE
        # DSA 1024 key uses SHA-1 as hash but verification would succeed till 2024
        if sign_key_size == 1024:
            return
        self._encrypt_decrypt(self.gpg, self.rnp)

    def do_test_decrypt(self, sign_key_size, enc_key_size):
        pfx = EncryptElgamal.key_pfx(sign_key_size, enc_key_size)
        self.operation_key_location = tuple((key_path(pfx, False), key_path(pfx, True)))
        self.rnp.userid = self.gpg.userid = pfx + AT_EXAMPLE
        if sign_key_size == 1024:
            return
        self._encrypt_decrypt(self.rnp, self.gpg)

    def test_encrypt_P1024_1024(self): self.do_test_encrypt(1024, 1024)
    def test_encrypt_P1024_2048(self): self.do_test_encrypt(1024, 2048)
    def test_encrypt_P2048_2048(self): self.do_test_encrypt(2048, 2048)
    def test_encrypt_P3072_3072(self): self.do_test_encrypt(3072, 3072)
    def test_decrypt_P1024_1024(self): self.do_test_decrypt(1024, 1024)
    def test_decrypt_P2048_2048(self): self.do_test_decrypt(2048, 2048)
    def test_decrypt_P1234_1234(self): self.do_test_decrypt(1234, 1234)

    # 1024-bit key generation test was removed since it uses SHA1, which is not allowed for key signatures since Jan 19, 2024.

    def test_generate_elgamal_key1536_in_gpg_and_encrypt(self):
        cmd = EncryptElgamal.GPG_GENERATE_DSA_ELGAMAL_PATTERN.format(1536, 1536, self.gpg.userid)
        self.operation_key_gencmd = cmd
        self._encrypt_decrypt(self.gpg, self.rnp)

    def test_generate_elgamal_key1024_in_rnp_and_decrypt(self):
        cmd = EncryptElgamal.RNP_GENERATE_DSA_ELGAMAL_PATTERN.format(1024)
        self.operation_key_gencmd = cmd
        self._encrypt_decrypt(self.rnp, self.gpg)


class EncryptEcdh(Encrypt):

    GPG_GENERATE_ECDH_ECDSA_PATTERN = """
        Key-Type: ecdsa
        Key-Curve: {0}
        Key-Usage: sign auth
        Subkey-Type: ecdh
        Subkey-Usage: encrypt
        Subkey-Curve: {0}
        Name-Real: Test Testovich
        Expire-Date: 1y
        Preferences: aes256 sha256 sha384 sha512 sha1 zlib
        Name-Email: {1}"""

    RNP_GENERATE_ECDH_ECDSA_PATTERN = "19\n{0}\n"

    def test_encrypt_nistP256(self):
        self.operation_key_gencmd = EncryptEcdh.GPG_GENERATE_ECDH_ECDSA_PATTERN.format(
            "nistp256", self.rnp.userid)
        self._encrypt_decrypt(self.gpg, self.rnp)

    def test_encrypt_nistP384(self):
        self.operation_key_gencmd = EncryptEcdh.GPG_GENERATE_ECDH_ECDSA_PATTERN.format(
            "nistp384", self.rnp.userid)
        self._encrypt_decrypt(self.gpg, self.rnp)

    def test_encrypt_nistP521(self):
        self.operation_key_gencmd = EncryptEcdh.GPG_GENERATE_ECDH_ECDSA_PATTERN.format(
            "nistp521", self.rnp.userid)
        self._encrypt_decrypt(self.gpg, self.rnp)

    def test_decrypt_nistP256(self):
        self.operation_key_gencmd = EncryptEcdh.RNP_GENERATE_ECDH_ECDSA_PATTERN.format(1)
        self._encrypt_decrypt(self.rnp, self.gpg)

    def test_decrypt_nistP384(self):
        self.operation_key_gencmd = EncryptEcdh.RNP_GENERATE_ECDH_ECDSA_PATTERN.format(2)
        self._encrypt_decrypt(self.rnp, self.gpg)

    def test_decrypt_nistP521(self):
        self.operation_key_gencmd = EncryptEcdh.RNP_GENERATE_ECDH_ECDSA_PATTERN.format(3)
        self._encrypt_decrypt(self.rnp, self.gpg)

class Sign(unittest.TestCase, TestIdMixin, KeyLocationChooserMixin):
    SIZES = [20, 1000, 5000, 20000, 150000, 1000000]

    def _sign_verify(self, e1, e2, failsign = False, failver = False):
        '''
        Helper function for Sign verification
        1. e1 creates/loads key
        2. e1 exports key
        3. e2 imports key
        2. e1 signs message
        3. e2 verifies message

        eX == entityX
        '''
        keyfile, src, output = reg_workfiles(self.test_id, '.gpg', '.in', '.out')
        random_text(src, 0x1337)

        if not self.operation_key_location and not self.operation_key_gencmd:
            print(self.operation_key_gencmd)
            raise RuntimeError("key not found")

        if self.operation_key_location:
            self.assertTrue(e1.import_key(self.operation_key_location[0]))
            self.assertTrue(e1.import_key(self.operation_key_location[1], True))
        else:
            self.assertTrue(e1.generate_key_batch(self.operation_key_gencmd))
        self.assertTrue(e1.export_key(keyfile, False))
        self.assertTrue(e2.import_key(keyfile))
        self.assertEqual(e1.sign(output, src), not failsign)
        self.assertEqual(e2.verify(output), not failver)
        clear_workfiles()

    def setUp(self):
        KeyLocationChooserMixin.__init__(self)
        self.rnp = Rnp(RNPDIR, RNP, RNPK)
        self.gpg = GnuPG(GPGHOME, GPG)
        self.rnp.password = self.gpg.password = PASSWORD
        self.rnp.userid = self.gpg.userid = self.test_id + AT_EXAMPLE

    @classmethod
    def tearDownClass(cls):
        clear_keyrings()

class SignECDSA(Sign):
    # {0} must be replaced by ID of the curve 3,4 or 5 (NIST-256,384,521)
    #CURVES = ["NIST P-256", "NIST P-384", "NIST P-521"]
    GPG_GENERATE_ECDSA_PATTERN = """
        Key-Type: ecdsa
        Key-Curve: {0}
        Key-Usage: sign auth
        Name-Real: Test Testovich
        Expire-Date: 1y
        Preferences: twofish sha512 zlib
        Name-Email: {1}"""

    # {0} must be replaced by ID of the curve 1,2 or 3 (NIST-256,384,521)
    RNP_GENERATE_ECDSA_PATTERN = "19\n{0}\n"

    def test_sign_P256(self):
        cmd = SignECDSA.RNP_GENERATE_ECDSA_PATTERN.format(1)
        self.operation_key_gencmd = cmd
        self._sign_verify(self.rnp, self.gpg)

    def test_sign_P384(self):
        cmd = SignECDSA.RNP_GENERATE_ECDSA_PATTERN.format(2)
        self.operation_key_gencmd = cmd
        self._sign_verify(self.rnp, self.gpg)

    def test_sign_P521(self):
        cmd = SignECDSA.RNP_GENERATE_ECDSA_PATTERN.format(3)
        self.operation_key_gencmd = cmd
        self._sign_verify(self.rnp, self.gpg)

    def test_verify_P256(self):
        cmd = SignECDSA.GPG_GENERATE_ECDSA_PATTERN.format("nistp256", self.rnp.userid)
        self.operation_key_gencmd = cmd
        self._sign_verify(self.gpg, self.rnp)

    def test_verify_P384(self):
        cmd = SignECDSA.GPG_GENERATE_ECDSA_PATTERN.format("nistp384", self.rnp.userid)
        self.operation_key_gencmd = cmd
        self._sign_verify(self.gpg, self.rnp)

    def test_verify_P521(self):
        cmd = SignECDSA.GPG_GENERATE_ECDSA_PATTERN.format("nistp521", self.rnp.userid)
        self.operation_key_gencmd = cmd
        self._sign_verify(self.gpg, self.rnp)

    def test_hash_truncation(self):
        '''
        Signs message hashed with SHA512 with a key of size 256. Implementation
        truncates leftmost 256 bits of a hash before signing (see FIPS 186-4, 6.4)
        '''
        cmd = SignECDSA.RNP_GENERATE_ECDSA_PATTERN.format(1)
        rnp = self.rnp.copy()
        rnp.hash = 'SHA512'
        self.operation_key_gencmd = cmd
        self._sign_verify(rnp, self.gpg)

class SignDSA(Sign):
    # {0} must be replaced by ID of the curve 3,4 or 5 (NIST-256,384,521)
    #CURVES = ["NIST P-256", "NIST P-384", "NIST P-521"]
    GPG_GENERATE_DSA_PATTERN = """
        Key-Type: dsa
        Key-Length: {0}
        Key-Usage: sign auth
        Name-Real: Test Testovich
        Expire-Date: 1y
        Preferences: twofish sha256 sha384 sha512 sha1 zlib
        Name-Email: {1}"""

    # {0} must be replaced by ID of the curve 1,2 or 3 (NIST-256,384,521)
    RNP_GENERATE_DSA_PATTERN = "17\n{0}\n"

    @staticmethod
    def key_pfx(p): return "GnuPG_dsa_elgamal_%d_%d" % (p, p)

    def do_test_sign(self, p_size):
        pfx = SignDSA.key_pfx(p_size)
        self.operation_key_location = tuple((key_path(pfx, False), key_path(pfx, True)))
        self.rnp.userid = self.gpg.userid = pfx + AT_EXAMPLE
        # DSA 1024-bit key uses SHA-1 so verification would not fail till 2024
        self._sign_verify(self.rnp, self.gpg)

    def do_test_verify(self, p_size):
        pfx = SignDSA.key_pfx(p_size)
        self.operation_key_location = tuple((key_path(pfx, False), key_path(pfx, True)))
        self.rnp.userid = self.gpg.userid = pfx + AT_EXAMPLE
        # DSA 1024-bit key uses SHA-1, but verification would fail since SHA1 is used by GnuPG
        self._sign_verify(self.gpg, self.rnp, False, p_size <= 1024)

    def test_sign_P1024_Q160(self): self.do_test_sign(1024)
    def test_sign_P2048_Q256(self): self.do_test_sign(2048)
    def test_sign_P3072_Q256(self): self.do_test_sign(3072)
    def test_sign_P2112_Q256(self): self.do_test_sign(2112)

    def test_verify_P1024_Q160(self): self.do_test_verify(1024)
    def test_verify_P2048_Q256(self): self.do_test_verify(2048)
    def test_verify_P3072_Q256(self): self.do_test_verify(3072)
    def test_verify_P2112_Q256(self): self.do_test_verify(2112)

    def test_sign_P1088_Q224(self):
        self.operation_key_gencmd = SignDSA.RNP_GENERATE_DSA_PATTERN.format(1088)
        self._sign_verify(self.rnp, self.gpg)

    def test_verify_P1088_Q224(self):
        self.operation_key_gencmd = SignDSA.GPG_GENERATE_DSA_PATTERN.format("1088", self.rnp.userid)
        self._sign_verify(self.gpg, self.rnp)

    def test_hash_truncation(self):
        '''
        Signs message hashed with SHA512 with a key of size 160 bits. Implementation
        truncates leftmost 160 bits of a hash before signing (see FIPS 186-4, 4.2)
        '''
        rnp = self.rnp.copy()
        rnp.hash = 'SHA512'
        self.operation_key_gencmd = SignDSA.RNP_GENERATE_DSA_PATTERN.format(1024)
        self._sign_verify(rnp, self.gpg)

class EncryptSignRSA(Encrypt, Sign):

    GPG_GENERATE_RSA_PATTERN = """
        Key-Type: rsa
        Key-Length: {0}
        Key-Usage: sign auth
        Subkey-Type: rsa
        Subkey-Length: {0}
        Subkey-Usage: encrypt
        Name-Real: Test Testovich
        Expire-Date: 1y
        Preferences: twofish sha256 sha384 sha512 sha1 zlib
        Name-Email: {1}"""

    RNP_GENERATE_RSA_PATTERN = "1\n{0}\n"

    @staticmethod
    def key_pfx(p): return "GnuPG_rsa_%d_%d" % (p, p)

    def do_encrypt_verify(self, key_size):
        pfx = EncryptSignRSA.key_pfx(key_size)
        self.operation_key_location = tuple((key_path(pfx, False), key_path(pfx, True)))
        self.rnp.userid = self.gpg.userid = pfx + AT_EXAMPLE
        self._encrypt_decrypt(self.gpg, self.rnp)
        self._sign_verify(self.gpg, self.rnp)

    def do_rnp_decrypt_sign(self, key_size):
        pfx = EncryptSignRSA.key_pfx(key_size)
        self.operation_key_location = tuple((key_path(pfx, False), key_path(pfx, True)))
        self.rnp.userid = self.gpg.userid = pfx + AT_EXAMPLE
        self._encrypt_decrypt(self.rnp, self.gpg)
        self._sign_verify(self.rnp, self.gpg)

    def test_rnp_encrypt_verify_1024(self): self.do_encrypt_verify(1024)
    def test_rnp_encrypt_verify_2048(self): self.do_encrypt_verify(2048)
    def test_rnp_encrypt_verify_4096(self): self.do_encrypt_verify(4096)

    def test_rnp_decrypt_sign_1024(self): self.do_rnp_decrypt_sign(1024)
    def test_rnp_decrypt_sign_2048(self): self.do_rnp_decrypt_sign(2048)
    def test_rnp_decrypt_sign_4096(self): self.do_rnp_decrypt_sign(4096)

    def setUp(self):
        Encrypt.setUp(self)

    @classmethod
    def tearDownClass(cls):
        Encrypt.tearDownClass()

def test_suites(tests):
    if hasattr(tests, '__iter__'):
        for x in tests:
            for y in test_suites(x):
                yield y
    else:
        yield tests.__class__.__name__

# Main thinghy

if __name__ == '__main__':
    main = unittest.main
    if not hasattr(main, 'USAGE'):
        main.USAGE = ''
    main.USAGE += ''.join([
                  "\nRNP test client specific flags:\n",
                  "  -w,\t\t Don't remove working directory\n",
                  "  -d,\t\t Enable debug messages\n"])

    LEAVE_WORKING_DIRECTORY = ("-w" in sys.argv)
    if LEAVE_WORKING_DIRECTORY:
        # -w must be removed as unittest doesn't expect it
        sys.argv.remove('-w')
    else:
        LEAVE_WORKING_DIRECTORY = os.getenv('RNP_KEEP_TEMP') is not None

    LVL = logging.INFO
    if "-d" in sys.argv:
        sys.argv.remove('-d')
        LVL = logging.DEBUG

    # list suites
    if '-ls' in sys.argv:
        tests = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
        for suite in set(test_suites(tests)):
            print(suite)
        sys.exit(0)

    setup(LVL)
    res = main(exit=False)

    if not LEAVE_WORKING_DIRECTORY:
        try:
            if RMWORKDIR:
                shutil.rmtree(WORKDIR)
            else:
                shutil.rmtree(RNPDIR)
                shutil.rmtree(GPGDIR)
        except Exception:
            # Ignore exception if something cannot be deleted
            pass

    sys.exit(not res.result.wasSuccessful())
