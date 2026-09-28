FROM ubuntu:26.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    aspell aspell-en ca-certificates ccache clang clang-format clang-tidy \
    clangd cmake curl doxygen ed file g++ gcc gdb gh git openssh-client \
    libclang-rt-21-dev liblz4-dev libsnappy-dev libsodium-dev libzstd-dev \
    lld lldb llvm make ninja-build python-is-python3 python3 python3-dev \
    python3-pip python3-venv swig universal-ctags zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash wiredtiger
USER wiredtiger

RUN python -m venv /home/wiredtiger/venv
ENV PATH="/home/wiredtiger/venv/bin:$PATH"

RUN pip install --no-cache-dir \
    find_libpython==0.4.0 gcovr==8.6 psutil==5.9.4 ruff==0.4.5 uv==0.11.18

ENV PATH="/home/wiredtiger/bin:$PATH"

RUN mkdir -p /home/wiredtiger/bin && \
    arch=$(dpkg --print-architecture) && \
    curl -fsSL "https://evg-bucket-evergreen.s3.amazonaws.com/evergreen/clients/latest/linux_${arch}/evergreen" \
    -o /home/wiredtiger/bin/evergreen && \
    chmod +x /home/wiredtiger/bin/evergreen

WORKDIR /workdir
