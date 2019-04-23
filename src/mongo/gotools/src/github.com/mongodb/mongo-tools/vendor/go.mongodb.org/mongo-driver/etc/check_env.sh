#!/bin/sh

if [[ ! $TOPOLOGY =~ ^(server|replica_set|sharded_cluster)$ ]]; then
    >&2 echo "Invalid value of TOPOLOGY. TOPOLOGY must be set to one of: server, replica_set, sharded_cluster"
    exit 1
fi

if [ ! -z $AUTH ] && [[ ! $AUTH =~ ^(noauth|auth)$ ]]; then
    >&2 echo "Invalid value of AUTH. AUTH can optionally be set to one of: noauth, auth"
    exit 1
fi

if [ ! -z $SSL ] && [[ ! $SLL =~ ^(nossl|ssl)$ ]]; then
    >&2 echo "Invalid value of SSL. SSL can optionally be set to one of: nossl, ssl"
    exit 1
fi

if [ ! -z $MONGO_GO_DRIVER_COMPRESSOR ] && [[ ! $MONGO_GO_DRIVER_COMPRESSOR =~ ^(snappy|zlib)$ ]]; then
    >&2 echo "Invalid value of MONGO_GO_DRIVER_COMPRESSOR. MONGO_GO_DRIVER_COMPRESSOR can optionally be set to one of: snappy, zlib"
    exit 1
fi