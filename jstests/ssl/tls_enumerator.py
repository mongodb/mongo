import argparse
import json
import socket
import ssl

exception_ciphers = {}


def enumerate_tls_ciphers(protocol_options, host, port, cert, cafile):
    root_context = ssl.SSLContext(ssl.PROTOCOL_TLS)
    root_context.options |= protocol_options
    root_context.set_ciphers("ALL:COMPLEMENTOFALL:-PSK:-SRP")

    ciphers = {cipher["name"] for cipher in root_context.get_ciphers()}

    accepted_ciphers = []

    for cipher_name in ciphers:
        context = ssl.SSLContext(root_context.protocol)
        try:
            context.set_ciphers(cipher_name)
        except ssl.SSLError as error:
            exception_ciphers[cipher_name] = str(error)
        context.options = root_context.options
        context.load_verify_locations(cafile=cafile)
        context.load_cert_chain(certfile=cert)

        with socket.socket(socket.AF_INET) as sock:
            with context.wrap_socket(sock, server_hostname=host) as conn:
                try:
                    conn.connect((host, port))
                except Exception:
                    continue
                accepted_ciphers.append(cipher_name)

    return sorted(accepted_ciphers)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MongoDB TLS Cipher Suite Enumerator")
    parser.add_argument("--port", type=int, default=27017, help="Port to connect to")
    parser.add_argument(
        "-o", "--outfile", type=str, default="ciphers.json", help="file to write the output to"
    )
    parser.add_argument("--host", type=str, default="localhost", help="host to connect to")
    parser.add_argument("--cafile", type=str, help="Path to CA certificate")
    parser.add_argument("--cert", type=str, help="Path to client certificate")
    args = parser.parse_args()

    # MacOS version of the toolchain does not have python linked with OpenSSL 1.1.1 yet, so we monkey patch this in here
    if not hasattr(ssl, "OP_NO_TLSv1_3"):
        ssl.OP_NO_TLSv1_3 = 0

    exclude_ops = {
        ssl.OP_NO_SSLv2,
        ssl.OP_NO_SSLv3,
        ssl.OP_NO_TLSv1,
        ssl.OP_NO_TLSv1_1,
        ssl.OP_NO_TLSv1_2,
        ssl.OP_NO_TLSv1_3,
    }

    def exclude_except(op):
        option = 0
        for other_op in exclude_ops - {op}:
            option |= other_op
        return option

    suites = {
        "sslv2": exclude_except(ssl.OP_NO_SSLv2),
        "sslv3": exclude_except(ssl.OP_NO_SSLv3),
        "tls1": exclude_except(ssl.OP_NO_TLSv1),
        "tls1_1": exclude_except(ssl.OP_NO_TLSv1_1),
        "tls1_2": exclude_except(ssl.OP_NO_TLSv1_2),
    }

    results = {
        key: enumerate_tls_ciphers(
            protocol_options=proto,
            host=args.host,
            port=args.port,
            cafile=args.cafile,
            cert=args.cert,
        )
        for key, proto in suites.items()
    }

    if exception_ciphers:
        print("System could not process the following ciphers")
        for cipher, error in exception_ciphers.items():
            print(cipher + "\tError: " + error)

    with open(args.outfile, "w+") as outfile:
        json.dump(results, outfile)
