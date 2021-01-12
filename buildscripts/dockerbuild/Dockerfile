FROM ubuntu:18.04
RUN apt update
RUN apt install software-properties-common -y
RUN apt update
RUN apt install git -y
RUN apt install python3.8 -y
RUN apt install python3-pip -y
RUN apt install libpython3.8-dev -y
RUN  ln -S -f /usr/bin/python3.8 /usr/bin/python3
RUN apt install g++-8 -y
RUN apt install gdb -y
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-8 800 --slave /usr/bin/g++ g++ /usr/bin/g++-8
RUN apt-get install libssl-dev -y
RUN apt-get install -y liblzma-dev
RUN apt-get install libcurl4-openssl-dev -y
# Copy and setup python dependencies as an optimization.
COPY pip /pip
RUN python3 -m pip install -r /pip/dev-requirements.txt

VOLUME mongo
CMD ["/bin/bash"]
