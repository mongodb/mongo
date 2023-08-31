FROM ubuntu:22.04

EXPOSE 27017
EXPOSE 27018
EXPOSE 27019

RUN mkdir -p /scripts
RUN mkdir -p /var/log/resmoke

RUN apt-get update

# Install mongo dependencies
RUN apt-get install -y curl

# Install git
RUN apt-get install -y git

# Set up Python
RUN apt-get install -y python3.10 python3-pip python3.10-venv
RUN ln -f $(which python3.10) /usr/bin/python
RUN ln -f $(which python3.10) /usr/bin/python3

RUN python -m pip install --upgrade pip wheel

# -------------------
# Everything above this line should be common image setup

# copy resmoke, make the venv, and pip install
COPY src /resmoke

RUN bash -c "cd /resmoke && python -m venv python3-venv && . python3-venv/bin/activate && pip install --upgrade pip wheel && pip install -r ./buildscripts/requirements.txt"

# copy the mongo binary -- make sure it is executable
COPY mongo /usr/bin
RUN chmod +x /usr/bin/mongo

COPY libvoidstar.so /usr/lib/libvoidstar.so

RUN /usr/bin/mongo --version
