#! /usr/bin/env python3

# This is a wrapper file around the SSLYze package
# The SSLYze package is used for testing the TLS/SSL
# suites that we allow for connections. 
import os, logging, json, argparse, urllib.parse
from sslyze.synchronous_scanner import SynchronousScanner
from sslyze.plugins import openssl_cipher_suites_plugin
from sslyze.plugins.openssl_cipher_suites_plugin import *
from sslyze.server_connectivity_tester import ServerConnectivityTester, ServerConnectivityError

path = str(os.path.dirname(os.path.abspath(__file__)))
filename = f'{path}/ciphers.json'

def server_scanner(p):
    try:
        logger = logging.getLogger(__name__)
        logger.info("Opening file")
        server_tester = ServerConnectivityTester(
            hostname = 'localhost',
            port=p
        )
        server_info = server_tester.perform()
        scanner = SynchronousScanner()

        results = dict()
        suite_names = ['SSLV2 Cipher Suites', 'SSLV3 Cipher Suites', 'TLSV1_0 Cipher Suites', 'TLSV1_1 Cipher Suites', 'TLSV1_2 Cipher Suites', 'TLSV1_3 Cipher Suites']
        suites = [Sslv20ScanCommand, Sslv30ScanCommand, Tlsv10ScanCommand, Tlsv11ScanCommand, Tlsv12ScanCommand, Tlsv13ScanCommand]
        
        logger.info("Scanning SSL/TLS suites, writing to ciphers.json")
        for suite_name, suite in zip(suite_names, suites):
            scan = scanner.run_scan_command(server_info, suite())
            results[suite_name] = [cipher.name for cipher in scan.accepted_cipher_list]
        
        with open(filename, 'w') as outfile:
            json.dump(results, outfile)

    except ServerConnectivityError as e:
        raise RuntimeError(f'Could not connect to {e.server_info.hostname}: {e.error_message}')

def main():
    parser = argparse.ArgumentParser(description='MongoDB SSL/TLS Suite Validation.')
    parser.add_argument('-p', '--port', type=int, default=27017, help="Port to listen on")
    parser.add_argument('-d', '--delete', action='store_true', help="Delete the ciphers.json file")
    args = parser.parse_args()
    if args.delete:
        os.remove(filename)
    else:
        return server_scanner(args.port)

if __name__ == '__main__':
    main()
