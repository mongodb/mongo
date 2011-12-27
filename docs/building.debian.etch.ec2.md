# Building on Debian etch on Amazon EC2

ami-f2f6159b

    apt-get update
    apt-get install git-core "g++-4.1"
    apt-get install python-setuptools libpcre3-dev
    apt-get install libboost-filesystem-dev libboost-dev libboost-thread-dev libboost-program-options-dev libboost-date-time-dev

See: http://www.mongodb.org/display/DOCS/Building+Spider+Monkey

    ln -s /usr/bin/g++-4.1 /usr/bin/g++
    ln -s /usr/bin/gcc-4.1 /usr/bin/gcc
 
    easy_install scons

    git clone git://github.com/mongodb/mongo.git
    cd mongo
    scons all