# Toolchain for every make target (see README, "Using it").
#   docker build -t sm64-gba .
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/work" sm64-gba sh -c 'make fetch && make test'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive HOME=/tmp
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc gcc-multilib libc6-dev-i386 gcc-arm-none-eabi libnewlib-arm-none-eabi \
        qemu-user libmgba-dev python3 python3-pil git ca-certificates make ffmpeg \
        g++ cmake \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
