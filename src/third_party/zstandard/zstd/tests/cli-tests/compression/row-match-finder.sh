#!/bin/sh

set -e

# Test --[no-]row-match-finder
zstd file -7f --row-match-finder
zstd file -7f --no-row-match-finder
