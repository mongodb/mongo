#!/usr/bin/env python

import sys
import tempfile
import shutil
import inspect
import os
import logging
from timeit import default_timer as perf_timer
from argparse import ArgumentParser
from cli_common import (
    find_utility,
    run_proc,
    run_proc_fast,
    pswd_pipe,
    rnp_file_path,
    size_to_readable,
    raise_err
)

RNP = ''
RNPK = ''
GPG = ''
WORKDIR = ''
RNPDIR = ''
GPGDIR = ''
RMWORKDIR = False
SMALL_ITERATIONS = 100
LARGE_ITERATIONS = 5
LARGESIZE = 1024*1024*100
SMALLSIZE = 0
SMALLFILE = 'smalltest.txt'
LARGEFILE = 'largetest.txt'
PASSWORD = 'password'

def setup(workdir):
    # Searching for rnp and gnupg
    global RNP, GPG, RNPK, WORKDIR, RNPDIR, GPGDIR, SMALLSIZE, RMWORKDIR
    logging.basicConfig(stream=sys.stdout, format="%(message)s")
    logging.getLogger().setLevel(logging.INFO)

    RNP = rnp_file_path('src/rnp/rnp')
    RNPK = rnp_file_path('src/rnpkeys/rnpkeys')
    GPG = find_utility('gpg')
    if workdir:
        WORKDIR = workdir
    else:
        WORKDIR = tempfile.mkdtemp(prefix = 'rnpptmp')
        RMWORKDIR = True

    logging.debug('Setting up test in {} ...'.format(WORKDIR))

    # Creating working directory and populating it with test files
    RNPDIR = os.path.join(WORKDIR, '.rnp')
    GPGDIR = os.path.join(WORKDIR, '.gpg')
    os.mkdir(RNPDIR, 0o700)
    os.mkdir(GPGDIR, 0o700)

    # Generating key
    pipe = pswd_pipe(PASSWORD)
    params = ['--homedir', RNPDIR, '--pass-fd', str(pipe), '--userid', 'performance@rnp',
              '--generate-key']
    # Run key generation
    run_proc(RNPK, params)
    os.close(pipe)


    # Importing keys to GnuPG so it can build trustdb and so on
    run_proc(GPG, ['--batch', '--passphrase', '', '--homedir', GPGDIR, '--import',
                   os.path.join(RNPDIR, 'pubring.gpg'), os.path.join(RNPDIR, 'secring.gpg')])

    # Generating small file for tests
    SMALLSIZE = 3312
    st = 'lorem ipsum dol ' * (SMALLSIZE//16+1)
    with open(os.path.join(WORKDIR, SMALLFILE), 'w+') as small_file:
        small_file.write(st)

    # Generating large file for tests
    print('Generating large file of size {}'.format(size_to_readable(LARGESIZE)))

    st = '0123456789ABCDEF' * (1024//16)
    with open(os.path.join(WORKDIR, LARGEFILE), 'w') as fd:
        for i in range(0, LARGESIZE // 1024):
            fd.write(st)

def run_iterated(iterations, func, src, dst, *args):
    runtime = 0

    for i in range(0, iterations):
        tstart = perf_timer()
        func(src, dst, *args)
        runtime += perf_timer() - tstart
        os.remove(dst)

    res = runtime / iterations
    #print '{} average run time: {}'.format(func.__name__, res)
    return res

def rnp_symencrypt_file(src, dst, cipher, zlevel = 6, zalgo = 'zip', armor = False):
    params = ['--homedir', RNPDIR, '--password', PASSWORD, '--cipher', cipher,
              '-z', str(zlevel), '--' + zalgo, '-c', src, '--output', dst]
    if armor:
        params += ['--armor']
    ret = run_proc_fast(RNP, params)
    if ret != 0:
        raise_err('rnp symmetric encryption failed')

def rnp_decrypt_file(src, dst):
    ret = run_proc_fast(RNP, ['--homedir', RNPDIR, '--password', PASSWORD, '--decrypt', src,
                              '--output', dst])
    if ret != 0:
        raise_err('rnp decryption failed')

def gpg_symencrypt_file(src, dst, cipher = 'AES', zlevel = 6, zalgo = 1, armor = False):
    params = ['--homedir', GPGDIR, '-c', '-z', str(zlevel), '--s2k-count', '524288',
              '--compress-algo', str(zalgo), '--batch', '--passphrase', PASSWORD,
              '--cipher-algo', cipher, '--output', dst, src]
    if armor:
        params.insert(2, '--armor')
    ret = run_proc_fast(GPG, params)
    if ret != 0:
        raise_err('gpg symmetric encryption failed for cipher ' + cipher)

def gpg_decrypt_file(src, dst, keypass):
    ret = run_proc_fast(GPG, ['--homedir', GPGDIR, '--pinentry-mode=loopback', '--batch',
                              '--yes', '--passphrase', keypass, '--trust-model', 'always',
                              '-o', dst, '-d', src])
    if ret != 0:
        raise_err('gpg decryption failed')

def print_test_results(fsize, rnptime, gpgtime, operation):
    if not rnptime or not gpgtime:
        logging.info('{}:TEST FAILED'.format(operation))

    if fsize == SMALLSIZE:
        rnpruns = 1.0 / rnptime
        gpgruns = 1.0 / gpgtime
        runstr = '{:.2f} runs/sec vs {:.2f} runs/sec'.format(rnpruns, gpgruns)

        if rnpruns >= gpgruns:
            percents = (rnpruns - gpgruns) / gpgruns * 100
            logging.info('{:<30}: RNP is {:>3.0f}% FASTER then GnuPG ({})'.format(
                operation, percents, runstr))
        else:
            percents = (gpgruns - rnpruns) / gpgruns * 100
            logging.info('{:<30}: RNP is {:>3.0f}% SLOWER then GnuPG ({})'.format(
                operation, percents, runstr))
    else:
        rnpspeed = fsize / 1024.0 / 1024.0 / rnptime
        gpgspeed = fsize / 1024.0 / 1024.0 / gpgtime
        spdstr = '{:.2f} MB/sec vs {:.2f} MB/sec'.format(rnpspeed, gpgspeed)

        if rnpspeed >= gpgspeed:
            percents = (rnpspeed - gpgspeed) / gpgspeed * 100
            logging.info('{:<30}: RNP is {:>3.0f}% FASTER then GnuPG ({})'.format(
                operation, percents, spdstr))
        else:
            percents = (gpgspeed - rnpspeed) / gpgspeed * 100
            logging.info('{:<30}: RNP is {:>3.0f}% SLOWER then GnuPG ({})'.format(
                operation, percents, spdstr))

def get_file_params(filetype):
    if filetype == 'small':
        infile, outfile, iterations, fsize = (SMALLFILE, SMALLFILE + '.gpg',
                                              SMALL_ITERATIONS, SMALLSIZE)
    else:
        infile, outfile, iterations, fsize = (LARGEFILE, LARGEFILE + '.gpg',
                                              LARGE_ITERATIONS, LARGESIZE)

    infile = os.path.join(WORKDIR, infile)
    rnpout = os.path.join(WORKDIR, outfile + '.rnp')
    gpgout = os.path.join(WORKDIR, outfile + '.gpg')
    return (infile, rnpout, gpgout, iterations, fsize)


class Benchmark(object):
    rnphome = ['--homedir', RNPDIR]
    gpghome = ['--homedir', GPGDIR]

    def small_file_symmetric_encryption(self):
        # Running each operation iteratively for a small and large file(s), calculating the average
        # 1. Encryption
        '''
        Small file symmetric encryption
        '''
        infile, rnpout, gpgout, iterations, fsize = get_file_params('small')
        for armor in [False, True]:
            tmrnp = run_iterated(iterations, rnp_symencrypt_file, infile, rnpout,
                                 'AES128', 0, 'zip', armor)
            tmgpg = run_iterated(iterations, gpg_symencrypt_file, infile, gpgout,
                                 'AES128', 0, 1, armor)
            testname = 'ENCRYPT-SMALL-{}'.format('ARMOR' if armor else 'BINARY')
            print_test_results(fsize, tmrnp, tmgpg, testname)

    def large_file_symmetric_encryption(self):
        '''
        Large file symmetric encryption
        '''
        infile, rnpout, gpgout, iterations, fsize = get_file_params('large')
        for cipher in ['AES128', 'AES192', 'AES256', 'TWOFISH', 'BLOWFISH', 'CAST5', 'CAMELLIA128', 'CAMELLIA192', 'CAMELLIA256']:
            tmrnp = run_iterated(iterations, rnp_symencrypt_file, infile, rnpout,
                                 cipher, 0, 'zip', False)
            tmgpg = run_iterated(iterations, gpg_symencrypt_file, infile, gpgout,
                                 cipher, 0, 1, False)
            testname = 'ENCRYPT-{}-BINARY'.format(cipher)
            print_test_results(fsize, tmrnp, tmgpg, testname)

    def large_file_armored_encryption(self):
        '''
        Large file armored encryption
        '''
        infile, rnpout, gpgout, iterations, fsize = get_file_params('large')
        tmrnp = run_iterated(iterations, rnp_symencrypt_file, infile, rnpout,
                             'AES128', 0, 'zip', True)
        tmgpg = run_iterated(iterations, gpg_symencrypt_file, infile, gpgout, 'AES128', 0, 1, True)
        print_test_results(fsize, tmrnp, tmgpg, 'ENCRYPT-LARGE-ARMOR')

    def small_file_symmetric_decryption(self):
        '''
        Small file symmetric decryption
        '''
        infile, rnpout, gpgout, iterations, fsize = get_file_params('small')
        inenc = infile + '.enc'
        for armor in [False, True]:
            gpg_symencrypt_file(infile, inenc, 'AES', 0, 1, armor)
            tmrnp = run_iterated(iterations, rnp_decrypt_file, inenc, rnpout)
            tmgpg = run_iterated(iterations, gpg_decrypt_file, inenc, gpgout, PASSWORD)
            testname = 'DECRYPT-SMALL-{}'.format('ARMOR' if armor else 'BINARY')
            print_test_results(fsize, tmrnp, tmgpg, testname)
            os.remove(inenc)

    def large_file_symmetric_decryption(self):
        '''
        Large file symmetric decryption
        '''
        infile, rnpout, gpgout, iterations, fsize = get_file_params('large')
        inenc = infile + '.enc'
        for cipher in ['AES128', 'AES192', 'AES256', 'TWOFISH', 'BLOWFISH', 'CAST5',
                       'CAMELLIA128', 'CAMELLIA192', 'CAMELLIA256']:
            gpg_symencrypt_file(infile, inenc, cipher, 0, 1, False)
            tmrnp = run_iterated(iterations, rnp_decrypt_file, inenc, rnpout)
            tmgpg = run_iterated(iterations, gpg_decrypt_file, inenc, gpgout, PASSWORD)
            testname = 'DECRYPT-{}-BINARY'.format(cipher)
            print_test_results(fsize, tmrnp, tmgpg, testname)
            os.remove(inenc)

    def large_file_armored_decryption(self):
        '''
        Large file armored decryption
        '''
        infile, rnpout, gpgout, iterations, fsize = get_file_params('large')
        inenc = infile + '.enc'
        gpg_symencrypt_file(infile, inenc, 'AES128', 0, 1, True)
        tmrnp = run_iterated(iterations, rnp_decrypt_file, inenc, rnpout)
        tmgpg = run_iterated(iterations, gpg_decrypt_file, inenc, gpgout, PASSWORD)
        print_test_results(fsize, tmrnp, tmgpg, 'DECRYPT-LARGE-ARMOR')
        os.remove(inenc)

        # 3. Signing
        #print '\n#3. Signing\n'
        # 4. Verification
        #print '\n#4. Verification\n'
        # 5. Cleartext signing
        #print '\n#5. Cleartext signing and verification\n'
        # 6. Detached signature
        #print '\n#6. Detached signing and verification\n'

# Usage ./cli_perf.py [working_directory]
#
# It's better to use RAMDISK to perform tests
# in order to speed up disk reads/writes
#
# On linux:
# mkdir -p /tmp/working
# sudo mount -t tmpfs -o size=512m tmpfs /tmp/working
# ./cli_perf.py -w /tmp/working
# sudo umount /tmp/working


if __name__ == '__main__':

    # parse options
    parser = ArgumentParser(description="RNP benchmarking")
    parser.add_argument("-b", "--bench", dest="benchmarks",
                      help="Name of the comma-separated benchmarks to run", metavar="benchmarks")
    parser.add_argument("-w", "--workdir", dest="workdir",
                      help="Working directory to use", metavar="workdir")
    parser.add_argument("-l", "--list", help="Print list of available benchmarks and exit",
                        action="store_true")
    args = parser.parse_args()

    # get list of benchmarks to run
    bench_methods = [ x[0] for x in inspect.getmembers(Benchmark,
            predicate=lambda x: inspect.ismethod(x) or inspect.isfunction(x))]
    print(bench_methods)

    if args.list:
        for name in bench_methods:
            logging.info(("\t " + name))
        sys.exit(0)

    if args.benchmarks:
        bench_methods = filter(lambda x: x in args.benchmarks.split(","), bench_methods)

    # setup operations
    setup(args.workdir)

    for name in bench_methods:
        method = getattr(Benchmark, name)
        logging.info(("\n" + name + "(): " + inspect.getdoc(method)))
        method(Benchmark())

    try:
        shutil.rmtree(WORKDIR)
    except Exception:
        logging.info(("Cleanup failed"))
