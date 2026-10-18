# Optional V8-enabled image. The default Dockerfile stays JavaScript-free.
# Build: docker build -f Dockerfile.javascript -t inspircd:javascript .
# Alpine's nodejs-libs supplies V8; this module does not start a Node environment.
# Keep build/runtime package versions equal. Update this pin for security fixes.
ARG NODEJS_VERSION=22.23.2-r0

FROM alpine:3.22 AS builder

ARG NODEJS_VERSION
ARG VERSION=v4.12.1
ARG CONFIGUREARGS=
ARG EXTRASMODULES=
ARG BUILD_DEPENDENCIES=

RUN apk add --no-cache gcc g++ make git pkgconfig perl binutils \
       perl-net-ssleay perl-crypt-ssleay perl-lwp-protocol-https \
       perl-libwww wget gnutls-dev "nodejs-dev=${NODEJS_VERSION}" \
       $BUILD_DEPENDENCIES

RUN addgroup -g 10000 -S inspircd && \
    adduser -u 10000 -h /inspircd/ -D -S -G inspircd inspircd

RUN git clone --depth 1 --branch "$VERSION" \
       https://github.com/inspircd/inspircd.git /inspircd-src

COPY modules/ /inspircd-src/src/modules/
WORKDIR /inspircd-src

RUN echo $EXTRASMODULES | xargs --no-run-if-empty ./modulemanager install
# Enabling extras is a separate configure operation which exits immediately.
RUN ./configure --enable-extras=javascript && \
    ./configure --prefix /inspircd --example-dir /inspircd/examples \
       --uid 10000 --gid 10000
RUN echo $CONFIGUREARGS | xargs --no-run-if-empty ./configure
RUN make -j"$(getconf _NPROCESSORS_ONLN)" install && \
    test -s /inspircd/modules/m_javascript.so && \
    readelf -d /inspircd/modules/m_javascript.so | grep -q 'libnode.so.127' && \
    readelf -d /inspircd/modules/m_javascript.so | grep -q NODELETE && \
    rm -rf /inspircd/conf/*

FROM alpine:3.22

ARG NODEJS_VERSION
ARG RUN_DEPENDENCIES=

RUN apk add --no-cache libgcc libstdc++ gnutls gnutls-utils \
       "nodejs-libs=${NODEJS_VERSION}" $RUN_DEPENDENCIES && \
    addgroup -g 10000 -S inspircd && \
    adduser -u 10000 -h /inspircd/ -D -S -G inspircd inspircd

COPY --chown=inspircd:inspircd conf/ /conf/
COPY --chown=inspircd:inspircd entrypoint.sh /entrypoint.sh
COPY --from=builder --chown=inspircd:inspircd /inspircd/ /inspircd/

USER inspircd
EXPOSE 6667 6697 7000 7001
WORKDIR /
ENTRYPOINT ["/entrypoint.sh"]

HEALTHCHECK --interval=60s --timeout=3s --start-period=60s --retries=3 \
    CMD /usr/bin/nc -z localhost 6667
