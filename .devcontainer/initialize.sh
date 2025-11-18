#!/bin/bash
# MongoDB Development Container Host Initialization Script
# This script runs on the host machine before the container starts.

set -euo pipefail

# Configure core dump pattern in the Docker VM
# This is a kernel-level setting that cannot be modified from within unprivileged containers,
# so we use nsenter to enter the Docker VM's mount namespace and set it there.
echo "Configuring core dump pattern in Docker VM..."
docker run --rm --privileged --pid=host \
    alpine:3.22.2@sha256:4b7ce07002c69e8f3d704a9c5d6fd3053be500b7f1c69fc0d80990c2ad8dd412 \
    nsenter -t 1 -m -- sh -c "echo 'dump_%e.%p.core' > /proc/sys/kernel/core_pattern" \
    2>/dev/null || {
    echo "Warning: Could not set core dump pattern (this is expected on non-Docker Desktop environments)"
}

echo "Host initialization complete"
