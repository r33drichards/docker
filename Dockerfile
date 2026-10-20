# Match the V8 headers and shared library in both stages.
ARG NODEJS_VERSION=22.23.2-r0

FROM alpine:3.22 AS builder

LABEL maintainer="InspIRCd Team <noreply@inspircd.org>"

ARG NODEJS_VERSION
ARG VERSION=v4.12.1
ARG CONFIGUREARGS=
ARG EXTRASMODULES=
ARG BUILD_DEPENDENCIES=
# CI-only synthetic hook driver; excluded from normal builds.
ARG TEST_HOOK_PROBE=0

# Stage 0: Build from source
COPY modules/ /src/modules/
COPY tests/fixtures/ /tmp/js-hook-fixtures/

RUN apk add --no-cache gcc g++ make git pkgconfig perl binutils curl-dev sqlite-dev openssl-dev openssl-libs-static \
       perl-net-ssleay perl-crypt-ssleay perl-lwp-protocol-https \
       perl-libwww wget gnutls-dev "nodejs-dev=${NODEJS_VERSION}" $BUILD_DEPENDENCIES

# Static bootstrap: no dynamic-loader environment or shell runs while root.
COPY tools/sqlite-bootstrap.c /tmp/sqlite-bootstrap.c
RUN cc -std=c11 -Os -static -Wall -Wextra -Werror \
    -o /sqlite-bootstrap /tmp/sqlite-bootstrap.c

COPY tools/seed-persistent-config.c /tmp/seed-persistent-config.c
RUN cc -std=c11 -Os -static -Wall -Wextra -Werror \
    -o /seed-persistent-config /tmp/seed-persistent-config.c $(pkg-config --static --libs libcrypto)

RUN addgroup -g 10000 -S inspircd
RUN adduser -u 10000 -h /inspircd/ -D -S -G inspircd inspircd

RUN git clone --branch $VERSION https://github.com/inspircd/inspircd.git inspircd-src

WORKDIR /inspircd-src
RUN git checkout $(git describe --abbrev=0 --tags $VERSION)

## Add modules
RUN { [ $(ls /src/modules/ | wc -l) -gt 0 ] && cp -r /src/modules/* /inspircd-src/src/modules/ || echo "No modules overwritten/added by repository"; }
RUN if [ "$TEST_HOOK_PROBE" = 1 ]; then cp /tmp/js-hook-fixtures/* /inspircd-src/src/modules/; fi
RUN echo $EXTRASMODULES | xargs --no-run-if-empty ./modulemanager install

# Enabling extras is a separate configure operation which exits immediately.
RUN ./configure --enable-extras=javascript
RUN ./configure --prefix /inspircd --example-dir /inspircd/examples --uid 10000 --gid 10000
RUN echo $CONFIGUREARGS | xargs --no-run-if-empty ./configure
RUN make -j`getconf _NPROCESSORS_ONLN` install && \
    test -s /inspircd/modules/m_javascript.so && \
    readelf -d /inspircd/modules/m_javascript.so | grep -q 'libnode.so.127' && \
    readelf -d /inspircd/modules/m_javascript.so | grep -q 'libsqlite3.so.0' && \
    readelf -d /inspircd/modules/m_javascript.so | grep -q NODELETE

## Wipe out vanilla config; entrypoint.sh will handle repopulating it at runtime
RUN mkdir -p /inspircd/conf /inspircd/data /inspircd/logs && rm -rf /inspircd/conf/*

# Stage 1: Create optimized runtime container
FROM alpine:3.22

ARG NODEJS_VERSION
ARG RUN_DEPENDENCIES=

RUN apk add --no-cache libgcc libstdc++ gnutls gnutls-utils libcurl sqlite-libs ca-certificates "nodejs-libs=${NODEJS_VERSION}" $RUN_DEPENDENCIES && \
    addgroup -g 10000 -S inspircd && \
    adduser -u 10000 -h /inspircd/ -D -S -G inspircd inspircd

COPY --chown=inspircd:inspircd conf/ /conf/
COPY --chown=inspircd:inspircd entrypoint.sh /entrypoint.sh
COPY --chown=inspircd:inspircd tools/materialize-nocodb-token.sh /materialize-nocodb-token.sh
COPY --from=builder /sqlite-bootstrap /sqlite-bootstrap
COPY --from=builder /seed-persistent-config /seed-persistent-config
COPY --chown=0:0 --chmod=0644 tools/seed-persistent-config.baseline /seed-persistent-config.baseline
COPY --from=builder --chown=inspircd:inspircd /inspircd/ /inspircd/
COPY --chown=inspircd:inspircd examples/javascript/wchat-nocodb.js /inspircd/scripts/wchat-nocodb.js

USER inspircd

EXPOSE 6667 6697 7000 7001

WORKDIR /
ENTRYPOINT ["/sqlite-bootstrap"]

HEALTHCHECK \
        --interval=60s \
        --timeout=3s \
        --start-period=60s \
        --retries=3 \
    CMD \
        /usr/bin/nc -z localhost 6667
