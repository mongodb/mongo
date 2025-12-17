#!/usr/bin/env bash

SCRIPT_DIR=$(dirname $(realpath "$BASH_SOURCE"))

CONNECTION_URL=$(bash /scripts/print_connection_string.sh)
/bin/mongo --nodb --eval "var connection_string=${CONNECTION_URL};load('$SCRIPT_DIR/js/commands.js'); insert();"
