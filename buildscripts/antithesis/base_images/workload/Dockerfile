FROM ubuntu:18.04

EXPOSE 27017
EXPOSE 27018
EXPOSE 27019

RUN mkdir -p /scripts
RUN mkdir -p /var/log/resmoke

# tzdata will block waiting for interactive input. Ensure that dpkg gets run
# in a non-interactive fashion by preseeding tzdata
COPY preseed.txt /tmp/preseed.txt
RUN debconf-set-selections /tmp/preseed.txt
RUN rm /tmp/preseed.txt

RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive DEBCONF_NONINTERACTIVE_SEEN=true apt-get install -qy git-all wget build-essential checkinstall libreadline-gplv2-dev libncursesw5-dev libssl-dev libsqlite3-dev tk-dev libgdbm-dev libc6-dev libbz2-dev libffi-dev zlib1g-dev
RUN apt-get install -qy libcurl4 libgssapi-krb5-2 libldap-2.4-2 libwrap0 libsasl2-2 libsasl2-modules libsasl2-modules-gssapi-mit openssl liblzma5 libssl-dev build-essential software-properties-common

# installs that need to be forced to be non-interactive: python 3.9 and git
RUN bash -c "cd /opt && wget https://www.python.org/ftp/python/3.9.16/Python-3.9.16.tgz && tar xzf Python-3.9.16.tgz && cd Python-3.9.16 && ./configure --enable-optimizations && make altinstall && rm -f /opt/Python-3.9.6.tgz"

# -------------------
# Everything above this line should be common image setup
# Everything below should be specific to the version of mongodb being installed

# copy resmoke, make the venv, and pip install
COPY src /resmoke

RUN bash -c "cd /resmoke && python3.9 -m venv python3-venv && . python3-venv/bin/activate && pip install --upgrade pip wheel && pip install -r ./buildscripts/requirements.txt"

# copy the mongo binary -- make sure it is executable
COPY mongo /usr/bin
RUN chmod +x /usr/bin/mongo

COPY libvoidstar.so /usr/lib/libvoidstar.so

RUN /usr/bin/mongo --version
