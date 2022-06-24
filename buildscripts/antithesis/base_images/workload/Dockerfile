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
RUN apt-get install -qy libcurl4 libgssapi-krb5-2 libldap-2.4-2 libwrap0 libsasl2-2 libsasl2-modules libsasl2-modules-gssapi-mit openssl liblzma5 libssl-dev build-essential software-properties-common
RUN add-apt-repository ppa:deadsnakes/ppa
RUN apt-get update

# installs that need to be forced to be non-interactive: python 3.9 and git
RUN DEBIAN_FRONTEND=noninteractive DEBCONF_NONINTERACTIVE_SEEN=true apt-get install -qy python3.9 python3.9-dev python3.9-venv git-all

# -------------------
# Everything above this line should be common image setup
# Everything below should be specific to the version of mongodb being installed

# copy resmoke, make the venv, and pip install
COPY src /resmoke

RUN bash -c "cd /resmoke && python3.9 -m venv python3-venv && . python3-venv/bin/activate && pip install --upgrade pip wheel && pip install -r ./buildscripts/requirements.txt && ./buildscripts/antithesis_suite.py generate-all"

# copy the run_suite.py script & mongo binary -- make sure they are executable
COPY run_suite.py /resmoke

COPY mongo /usr/bin
RUN chmod +x /usr/bin/mongo

COPY libvoidstar.so /usr/lib/libvoidstar.so

RUN /usr/bin/mongo --version
