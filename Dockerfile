FROM alpine:3.3

ADD . /code

WORKDIR /code

RUN apk add --no-cache curl tar scons g++ openssl-dev

RUN scons all -j$(getconf _NPROCESSORS_ONLN) --disable-warnings-as-errors \
    && scons --prefix=/opt/mongo install

VOLUME /data/db

COPY docker-entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]

EXPOSE 27017
CMD ["mongod"]
