FROM alpine:3.3

ADD . /code

WORKDIR /code

RUN ldd --version || true

RUN apk add --no-cache curl tar scons g++ linux-headers openssl-dev
RUN scons mongod -j$(getconf _NPROCESSORS_ONLN) --ssl --disable-warnings-as-errors
RUN scons mongo -j$(getconf _NPROCESSORS_ONLN) --ssl --disable-warnings-as-errors
RUN scons mongos-j$(getconf _NPROCESSORS_ONLN) --ssl --disable-warnings-as-errors
RUN scons dbtest -j$(getconf _NPROCESSORS_ONLN) --ssl --disable-warnings-as-errors
RUN scons unittests -j$(getconf _NPROCESSORS_ONLN) --ssl --disable-warnings-as-errors

RUN scons --prefix=/opt/mongo install

VOLUME /data/db

COPY docker-entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/entrypoint.sh"]

EXPOSE 27017
CMD ["mongod"]
