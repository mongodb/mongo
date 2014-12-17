#!/bin/sh

sudo mkdir /mnt/data
sudo ln -s /mnt/data /data
sudo chown hadoop /mnt/data

sudo easy_install pymongo
