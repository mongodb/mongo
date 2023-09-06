FROM ubuntu:22.04

# Expose a port range because the mongo binaries that are eventually started using this
# image will use the ports determined by resmoke. Resmoke determines the port to use
# for the mongo{d,s} processes by starting at 20000 and incrementing the port number
# for every new mongo{d,s} process.
EXPOSE 20000-20100

# prep the environment
RUN mkdir -p /data/db
RUN mkdir -p /data/configdb
RUN mkdir -p /var/log/mongodb
RUN mkdir -p /scripts

# Install dependencies of MongoDB Server
RUN apt-get update
RUN apt-get install -y curl python3

# -------------------
# Everything above this line should be common image setup
# Everything below should be specific to the version of mongodb being installed

COPY dist-test/bin/* /usr/bin/
RUN chmod +x /usr/bin/*
COPY libvoidstar.so /usr/lib/libvoidstar.so

RUN /usr/bin/mongo --version
