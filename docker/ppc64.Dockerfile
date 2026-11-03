# syntax=docker/dockerfile:1.7
#
# Pruebas del recompilador en la Mac: el núcleo compilado para Linux ppc64
# big-endian (ELFv1, la misma ABI que PSL1GHT en la PS3) y ejecutado con
# qemu-user. Es la única forma de correr en el host el código que genera el
# recompilador: RPCS3 no ejecuta código generado (docs/rpcs3.md).
#
# Dos imágenes (tools/docker-image-ppc64.sh):
#   --target cross  salsa64-ppc64-cross:dev, linux/amd64: Debian no trae
#                   gcc-powerpc64-linux-gnu para arm64; corre con la
#                   emulación de amd64 de Docker y compila en segundos.
#   --target qemu   salsa64-qemu:dev, nativa (arm64): qemu-ppc64 dentro de
#                   un contenedor amd64 se cuelga; nativo es rápido.

ARG DEBIAN=debian:12-slim

FROM --platform=linux/amd64 ${DEBIAN} AS cross
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc-powerpc64-linux-gnu libc6-dev-ppc64-cross \
      binutils-powerpc64-linux-gnu make \
 && rm -rf /var/lib/apt/lists/*
# zlib para ppc64 (las pruebas unitarias enlazan ziprom.c con -lz): Debian no
# trae binarios de ppc64 big-endian, así que se compila estática desde el
# paquete fuente de Debian (firmado) con el compilador cruzado.
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends dpkg-dev \
 && mkdir /tmp/zlib && cd /tmp/zlib && apt-get source --download-only zlib \
 && tar -xf zlib_*.orig.tar.* --strip-components=1 \
 && CC=powerpc64-linux-gnu-gcc AR=powerpc64-linux-gnu-ar RANLIB=powerpc64-linux-gnu-ranlib \
    ./configure --static --prefix=/usr/powerpc64-linux-gnu \
 && (make libz.a || make libz.a || make libz.a) && make install \
 && cd / && rm -rf /tmp/zlib && apt-get purge -y dpkg-dev && apt-get autoremove -y \
 && rm -rf /var/lib/apt/lists/*

FROM ${DEBIAN} AS qemu
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends qemu-user-static \
 && rm -rf /var/lib/apt/lists/*
