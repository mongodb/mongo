FROM       base
MAINTAINER Russell Smith "russ@rainforestqa.com"

ENV HOME /root

RUN apt-get update
RUN apt-get install -y scons build-essential
RUN apt-get install -y libboost-filesystem-dev libboost-program-options-dev libboost-system-dev libboost-thread-dev
RUN apt-get install -y python-pymongo

ADD ./ /root/mongo/

RUN cd /root/mongo/ && scons all -j `nproc`

ENTRYPOINT export HOME=/root;/bin/bash
