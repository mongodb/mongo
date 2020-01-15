#!/usr/bin/env bash

if [ ! -z $AUTH ] && [[ ! $AUTH =~ ^(noauth|auth)$ ]]; then
    >&2 echo "Invalid value of AUTH. AUTH can optionally be set to one of: noauth, auth"
    exit 1
fi

if [ ! -z $SSL ] && [[ ! $SSL =~ ^(nossl|ssl)$ ]]; then
    >&2 echo "Invalid value of SSL. SSL can optionally be set to one of: nossl, ssl"
    exit 1
fi

if [ ! -z $MONGO_GO_DRIVER_COMPRESSOR ] && [[ ! $MONGO_GO_DRIVER_COMPRESSOR =~ ^(snappy|zlib|zstd)$ ]]; then
    >&2 echo "Invalid value of MONGO_GO_DRIVER_COMPRESSOR. MONGO_GO_DRIVER_COMPRESSOR can optionally be set to one of: snappy, zlib, zstd"
    exit 1
fi
