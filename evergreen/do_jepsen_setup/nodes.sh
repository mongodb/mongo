DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
activate_venv
$python -c 'import socket; num_nodes = 5; print("\n".join(["%s:%d" % (socket.gethostname(), port) for port in range(20000, 20000 + num_nodes)]))' > nodes.txt
