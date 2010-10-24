#!/usr/bin/env python

from __future__ import with_statement
from libcloud.types import Provider
from libcloud.providers import get_driver
from libcloud.drivers.ec2 import EC2NodeDriver, NodeImage
from libcloud.base import Node, NodeImage, NodeSize, NodeState

# libcloud's SSH client seems to be one of those pointless wrappers
# that (at the moment) both doesn't add anything to the thing it wraps
# (Paramiko) and also fails to expose the underlying thing's features.
# What's wrong with people?
#from libcloud.ssh import SSHClient

import time
import sys
import settings
import subprocess
import os
import socket

EC2 = get_driver(Provider.EC2)
EC2Driver=EC2NodeDriver(settings.id, settings.key)

def tryEC2():

    image=NodeImage('ami-bf07ead6', 'ubuntu 10.4', EC2)
    size=NodeSize('m1.large', 'large', None, None, None, None, EC2)

    node = None
    try:
        node = EC2Driver.create_node(image=image, name="ubuntu-test", size=size, keyname="kp1", securitygroup=['default', 'dist-slave', 'buildbot-slave'])
        print node
        print node.id
        while node.state == NodeState.PENDING: 
            time.sleep(3)
    finally:
        if node:
            node.destroy()


class node(object):
    def initWait(self):
        while 1:
            n=None
            # EC2 sometimes takes a while to report a node.
            for i in range(6):
                nodes = [n for n in self.list_nodes() if (n.id==self.node.id)]
                if len(nodes)>0:
                    n=nodes[0]
                    break
                else:
                    time.sleep(10)
            if not n:
                raise Exception("couldn't find node with id %s" % self.node.id)
            if n.state == NodeState.PENDING: 
                time.sleep(10)
            else:
                self.node = n
                break
        print "ok"
        # Now wait for the node's sshd to be accepting connections.
        print "waiting for ssh"
        sshwait = True
        if sshwait == False:
            return
        while sshwait:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                try:
                    s.connect((self.node.public_ip[0], 22))
                    sshwait = False
                    print "connected on port 22 (ssh)"
                    time.sleep(15) # arbitrary timeout, in case the
                    # remote sshd is slow.
                except socket.error, err:
                    pass
            finally:
                s.close()
                time.sleep(3) # arbitrary timeout
        print "ok"

    def __enter__(self):
        return self
        
    def __exit__(self, arg0, arg1, arg2):
        print "shutting down node %s" % self.node
        self.node.destroy()

# I don't think libcloud's Nodes implement __enter__ and __exit__, and
# I like the with statement for ensuring that we don't leak nodes when
# we don't have to.
class ec2node(node):
    def list_nodes(self):
        return EC2Driver.list_nodes()

class ubuntuNode(ec2node):
    def __init__(self):
        image=NodeImage('ami-bf07ead6', 'ubuntu 10.4', EC2)
        size=NodeSize('m1.large', 'large', None, None, None, None, EC2)

        self.node = EC2Driver.create_node(image=image, name="ubuntu-test", size=size, securitygroup=['default', 'dist-slave', 'buildbot-slave'], keyname='kp1')

class centosNode(ec2node):
    def __init__(self):
        image=NodeImage('ami-ccb35ea5', 'ubuntu 10.4', EC2)
        size=NodeSize('m1.large', 'large', None, None, None, None, EC2)

        self.node = EC2Driver.create_node(image=image, name="ubuntu-test", size=size, securitygroup=['default', 'dist-slave', 'buildbot-slave'], keyname='kp1')

class rackspaceNode(node):
    def list_nodes(self):
        self.conn.list_nodes()

class fedora11Node(rackspaceNode):
    def __init__(self):
        driver = get_driver(Provider.RACKSPACE)
        self.conn = driver(settings.rackspace_account, settings.rackspace_api_key)
        string='Fedora 11'
        images=filter(lambda x: (x.name.find(string) > -1), self.conn.list_images())
        sizes=self.conn.list_sizes()
        sizes.sort(cmp=lambda x,y: int(x.ram)<int(y.ram))
        node = None
        if len(images) != 1:
            raise "too many images with \"%s\" in the name" % string
        image = images[0]
        self.node = self.conn.create_node(image=image, name=string, size=sizes[0])
        print self.node
        self.password = self.node.extra['password']

class Err(Exception):
    pass

def merge_yum_repo(dir, outdir):
    dirtail=dir.rstrip('\/').split('/')[-1]
    keyfile=settings.makedist['ssh_keyfile']
    makeyumrepo="""find . -name RPMS | while read dir; do (cd $dir/.. && createrepo .); done"""
    with centosNode() as centos:
        centos.initWait()
        print centos.node
        run_for_effect(["scp", "-o", "StrictHostKeyChecking no","-i", keyfile, "-r", dir, "root@"+centos.node.public_ip[0]+":"])
        run_for_effect(["ssh", "-o", "StrictHostKeyChecking no","-i", keyfile, "root@"+centos.node.public_ip[0], "cd ./" + dirtail + " && " + makeyumrepo])
        run_for_effect(["scp", "-o", "StrictHostKeyChecking no", "-i", keyfile, "-r", "root@"+centos.node.public_ip[0]+":./"+dirtail +'/*', outdir])

        

def merge_apt_repo(dir, outdir):
    dirtail=dir.rstrip('\/').split('/')[-1]

    gpgdir=settings.makedist['gpg_homedir']
    keyfile=settings.makedist['ssh_keyfile']

    makeaptrepo="""for x in debian ubuntu; do (cd $x; for d in `find . -name *.deb | sed 's|^./||; s|/[^/]*$||'  | sort -u`; do dpkg-scanpackages $d > $d/Packages; gzip -9c $d/Packages > $d/Packages.gz; done) ; done"""
    makereleaseprologue="""Origin: 10gen
Label: 10gen
Suite: 10gen
Codename: VVVVVV
Version: VVVVVV
Architectures: i386 amd64
Components: 10gen
Description: 10gen packages"""
    makeaptrelease="""find . -maxdepth 3 -mindepth 3 | while read d; do ( cd $d && (echo '%s' | sed s/VVVVVV/$(basename $(pwd))/;  apt-ftparchive release .) > /tmp/Release && mv /tmp/Release .  && gpg -r `gpg  --list-keys | grep uid | awk '{print $(NF)}'` --no-secmem-warning --no-tty -abs --output Release.gpg Release ); done""" % makereleaseprologue
    with ubuntuNode() as ubuntu:
        ubuntu.initWait()
        print ubuntu.node
        run_for_effect(["ssh", "-o", "StrictHostKeyChecking no","-i", keyfile, "ubuntu@"+ubuntu.node.public_ip[0], "sudo", "sh", "-c", "\"export DEBIAN_FRONTEND=noninteractive; apt-get update; apt-get -y install debhelper\""])
        run_for_effect(["scp", "-o", "StrictHostKeyChecking no","-i", keyfile, "-r", dir, "ubuntu@"+ubuntu.node.public_ip[0]+":"])        
        run_for_effect(["scp", "-o", "StrictHostKeyChecking no","-i", keyfile, "-r", gpgdir, "ubuntu@"+ubuntu.node.public_ip[0]+":.gnupg"])
        run_for_effect(["ssh", "-o", "StrictHostKeyChecking no","-i", keyfile, "ubuntu@"+ubuntu.node.public_ip[0], "sh", "-c",  "\"ls -lR ./" + dirtail + "\""])
        run_for_effect(["ssh", "-o", "StrictHostKeyChecking no","-i", keyfile, "ubuntu@"+ubuntu.node.public_ip[0], "cd ./"+dirtail + " && " + makeaptrepo])
        run_for_effect(["ssh", "-o", "StrictHostKeyChecking no","-i", keyfile, "ubuntu@"+ubuntu.node.public_ip[0], "cd ./"+dirtail + " && " + makeaptrelease])
        run_for_effect(["scp", "-o", "StrictHostKeyChecking no", "-i", keyfile, "-r", "ubuntu@"+ubuntu.node.public_ip[0]+":./"+dirtail +'/*', outdir])


def run_for_effect(argv):
    print " ".join(argv)
    r=subprocess.Popen(argv).wait()
    if r!=0:
        raise Err("subprocess %s exited %d" % (argv, r))

if __name__ == "__main__":
    (flavor, dir, outdir) = sys.argv[-3:]

    if flavor == "deb":
        merge_apt_repo(dir, outdir)
    elif flavor == "rpm":
        merge_yum_repo(dir, outdir)
    else:
        Err("unknown pkg flavor %s" % flavor)
    # TODO: yum repositories

    
    #main()
    #tryRackSpace()
