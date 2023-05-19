FROM ubuntu
MAINTAINER me@gmail.com
WORKDIR /opt
RUN mkdir -p bin/test/format
RUN mkdir -p /data/RUNDIR
COPY cmake_build/test/format/t bin/test/format/
COPY tools/voidstar/lib/libvoidstar.so tools/voidstar/lib/
COPY cmake_build/libwiredtiger.so.11.2.0 bin/
COPY cmake_build/wt bin/
RUN mkdir -p bin/ext/encryptors/rotn
COPY cmake_build/ext/encryptors/rotn/libwiredtiger_rotn.so bin/ext/encryptors/rotn
RUN mkdir -p bin/ext/collators/reverse
COPY cmake_build/ext/collators/reverse/libwiredtiger_reverse_collator.so bin/ext/collators/reverse
RUN mkdir -p bin/ext/collators/revint
COPY cmake_build/ext/collators/revint/libwiredtiger_revint_collator.so bin/ext/collators/revint
RUN mkdir -p bin/ext/compressors/snappy
COPY cmake_build/ext/compressors/snappy/libwiredtiger_snappy.so bin/ext/compressors/snappy
RUN mkdir -p bin/ext/compressors/lz4
COPY cmake_build/ext/compressors/lz4/libwiredtiger_lz4.so bin/ext/compressors/lz4
RUN mkdir -p bin/ext/compressors/zlib
COPY cmake_build/ext/compressors/zlib/libwiredtiger_zlib.so bin/ext/compressors/zlib
RUN mkdir -p bin/ext/compressors/zstd
COPY cmake_build/ext/compressors/zstd/libwiredtiger_zstd.so bin/ext/compressors/zstd
COPY tools/antithesis/test.sh bin/
COPY cmake_build/VERSION /data/
RUN apt-get update
RUN apt-get install -y libsnappy-dev gdb lz4 zstd
RUN echo "export LD_LIBRARY_PATH=/opt/bin:/opt/tools/voidstar/lib:$LD_LIBRARY_PATH" >> /root/.bashrc
