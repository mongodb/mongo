
Building on open solaris on ec2
================

ami-4133d528


pkg install SUNWgcc
pkg install SUNWgit
pkg install SUNWpython-setuptools

easy_install-2.4 scons


git clone git://github.com/mongodb/mongo.git
cd mongo
scons 
