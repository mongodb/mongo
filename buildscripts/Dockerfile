FROM       ubuntu:12.10
MAINTAINER Russell Smith "russ@rainforestqa.com"

RUN apt-get update
RUN apt-get install -y scons build-essential
RUN apt-get install -y libboost-filesystem-dev libboost-program-options-dev libboost-system-dev libboost-thread-dev
RUN apt-get install -y python-pymongo

RUN mkdir -p /usr/src/mongo
WORKDIR /usr/src/mongo
ADD ../ .

RUN cd scons all -j `nproc`

ENTRYPOINT /bin/bash
