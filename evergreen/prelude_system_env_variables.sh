#!/bin/bash

get_glibc_version() {
  getconf GNU_LIBC_VERSION | cut -d ' ' -f 2
}

# Systems with glibc 2.34 or newer register custom rseq ABI
# behavior that is incompatible with the new TCMalloc, and will cause
# TCMalloc's rseq functionality to break and fall back to the per-thread
# cache behavior. Systems with an older glibc version will successfully
# use TCMalloc's per-CPU caches. We must ensure this environment variable is
# set on problematic systems.
configure_glibc_pthread_req() {
  if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    local ver="$(get_glibc_version)"
    local major="$(echo $ver | cut -d '.' -f 1)"
    local minor="$(echo $ver | cut -d '.' -f 2)"
    if ((major > 2 || ((major == 2 && minor >= 34)))); then
      export GLIBC_TUNABLES="glibc.pthread.rseq=0"
      echo "glibc version >= 2.34 detected, setting env variable GLIBC_TUNABLES=glibc.pthread.rseq=0"
    fi
  fi
}

configure_glibc_pthread_req
