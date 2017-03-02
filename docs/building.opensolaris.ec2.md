# Building on open solaris on ec2

This assumes you are using ami-4133d528 (which is 32bit).


    pkg install SUNWgcc
    pkg install SUNWgit
    pkg install SUNWpython-setuptools
    easy_install-2.4 scons
    git clone git://github.com/bongodb/bongo.git
    cd bongo
    scons