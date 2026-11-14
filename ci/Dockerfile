FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5 AS asterisk
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config python3 ca-certificates curl patch bzip2 \
    libedit-dev libjansson-dev libsqlite3-dev libssl-dev libxml2-dev \
    uuid-dev libncurses-dev libopencore-amrnb-dev libopencore-amrwb-dev libvo-amrwbenc-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/src
RUN curl --fail --location --retry 3 -o asterisk.tar.gz \
      https://downloads.asterisk.org/pub/telephony/asterisk/old-releases/asterisk-22.11.0.tar.gz \
    && echo '3bd5ee040509a3d3cd9b1ba9520c18e6ec0a7e7981ca68c457dcd36ba3c54d94  asterisk.tar.gz' | sha256sum -c - \
    && tar xf asterisk.tar.gz && rm asterisk.tar.gz
WORKDIR /opt/src/asterisk-22.11.0
COPY patches/asterisk-22.11-stale-registration.patch /opt/src/stale-registration.patch
RUN patch -p1 < /opt/src/stale-registration.patch
RUN mkdir -p /opt/src/cache \
    && curl --fail --location --retry 3 -o /opt/src/cache/pjproject-2.17.tar.bz2 \
      https://raw.githubusercontent.com/asterisk/third-party/master/pjproject/2.17/pjproject-2.17.tar.bz2 \
    && echo '04b2eb1f0f01aa0ad1945b167171843448a51aa6b7c3e806496d434f13a112b7  /opt/src/cache/pjproject-2.17.tar.bz2' | sha256sum -c - \
    && ./configure --prefix=/opt/asterisk --libdir=/opt/asterisk/lib --with-pjproject-bundled --without-jansson-bundled --with-externals-cache=/opt/src/cache \
    && make menuselect.makeopts \
    && menuselect/menuselect --disable BUILD_NATIVE menuselect.makeopts \
    && make -j4 && make install && make samples
ENV ASTERISK_SRC=/opt/src/asterisk-22.11.0 \
    ASTERISK_PREFIX=/opt/asterisk
WORKDIR /work

FROM asterisk AS test
COPY . /work
RUN make test
RUN make package
