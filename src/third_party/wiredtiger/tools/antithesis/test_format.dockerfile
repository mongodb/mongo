FROM ubuntu
MAINTAINER me@gmail.com
WORKDIR /opt
RUN mkdir -p bin/test/format
RUN mkdir -p /data/RUNDIR
COPY cmake_build/test/format/t bin/test/format/
COPY tools/voidstar/lib/libvoidstar.so tools/voidstar/lib/
COPY cmake_build/libwiredtiger.so.11.2.0 bin/
RUN mkdir -p bin/ext/encryptors/rotn
COPY cmake_build/ext/encryptors/rotn/libwiredtiger_rotn.so bin/ext/encryptors/rotn
RUN mkdir -p bin/ext/collators/reverse
COPY cmake_build/ext/collators/reverse/libwiredtiger_reverse_collator.so bin/ext/collators/reverse
RUN mkdir -p bin/ext/collators/revint
COPY cmake_build/ext/collators/revint/libwiredtiger_revint_collator.so bin/ext/collators/revint
RUN mkdir -p bin/ext/compressors/snappy
COPY cmake_build/ext/compressors/snappy/libwiredtiger_snappy.so bin/ext/compressors/snappy
COPY tools/antithesis/test.sh bin/
COPY cmake_build/VERSION /data/
RUN apt-get update
RUN apt-get install -y libsnappy-dev
