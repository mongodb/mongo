#!/bin/bash

set -o errexit

# Extract artifacts from either zstd or gzip compressed tarballs
tar --zstd -xf fetched_artifacts.zst || tar -xf fetched_artifacts.tgz
