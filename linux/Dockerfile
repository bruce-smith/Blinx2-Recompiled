# The Linux build environment for blinx2-recomp: Ubuntu 24.04 with the
# toolchain and the libraries the runtime links (SDL2 for window, input and
# audio; the Vulkan loader headers; OpenSSL; GL/epoxy for xboxrecomp's d3d8
# layer). No game data goes into this image: the sources are mounted
# (see build-in-docker.sh).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build clang pkg-config git ca-certificates \
      libsdl2-dev libvulkan-dev libssl-dev libepoxy-dev libgl-dev \
    && rm -rf /var/lib/apt/lists/*
