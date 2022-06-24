FROM ubuntu:18.04

EXPOSE 27017
EXPOSE 27018
EXPOSE 27019

# prep the environment
RUN mkdir -p /data/db
RUN mkdir -p /data/configdb
RUN mkdir -p /var/log/mongodb
RUN mkdir -p /scripts

# Install dependencies of MongoDB Server
RUN apt-get update
RUN apt-get install -qy libcurl4 libgssapi-krb5-2 libldap-2.4-2 libwrap0 libsasl2-2 libsasl2-modules libsasl2-modules-gssapi-mit openssl liblzma5 python3

# -------------------
# Everything above this line should be common image setup
# Everything below should be specific to the version of mongodb being installed

COPY dist-test/bin/* /usr/bin/
RUN chmod +x /usr/bin/*
COPY libvoidstar.so /usr/lib/libvoidstar.so

RUN /usr/bin/mongo --version
