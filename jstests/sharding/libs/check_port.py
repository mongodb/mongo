"""check_port.py - Try to connect to an IPv4 "ip:port"

Usage: python check_port.py <IP> <PORT>

On success, exits with status zero.
On failure, exits with the status equal to the resulting [errno][1].

[1]: https://docs.python.org/3/library/errno.html
"""


import errno
import json
import socket
import sys


if __name__ == '__main__':
    ip, port = None, None
    try:
        ip = sys.argv[1]
        port = int(sys.argv[2])
    except Exception as error:
        args = json.dumps(sys.argv[1:])
        print(f'check_port.py: invalid command line arguments: {args}', file=sys.stderr)
        sys.exit(errno.EINVAL)

    try:
        sock = socket.socket()
        sock.connect((ip, port))
        sock.close()
    except OSError as error:
        sys.exit(error.errno)
