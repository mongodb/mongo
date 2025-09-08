#!/bin/bash
# This script downloads and imports protobuf

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=protobuf
VERSION="mongo/v4.25.0"
