FROM ubuntu:24.04

ENV PYTHONUNBUFFERED 1
ENV PYTHONPATH /tmp/openpilot:$PYTHONPATH

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    make \
    bzip2 \
    ca-certificates \
    capnproto \
    clang \
    g++ \
    gcc-arm-none-eabi libnewlib-arm-none-eabi \
    git \
    libarchive-dev \
    libbz2-dev \
    libcapnp-dev \
    libffi-dev \
    libtool \
    libusb-1.0-0 \
    libzmq3-dev \
    locales \
    opencl-headers \
    ocl-icd-opencl-dev \
    python3 \
    python3-dev \
    python3-pip \
    python-is-python3 \
    zlib1g-dev \
 && rm -rf /var/lib/apt/lists/* && \
    apt clean && \
    cd /usr/lib/gcc/arm-none-eabi/* && \
    rm -rf arm/ && \
    rm -rf thumb/nofp thumb/v6* thumb/v8* thumb/v7+fp thumb/v7-r+fp.sp

RUN sed -i -e 's/# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen && locale-gen
ENV LANG en_US.UTF-8
ENV LANGUAGE en_US:en
ENV LC_ALL en_US.UTF-8

COPY requirements.txt /tmp/
RUN pip3 install --break-system-packages --no-cache-dir -r /tmp/requirements.txt

ENV CPPCHECK_DIR=/tmp/cppcheck
COPY tests/misra/install.sh /tmp/
RUN /tmp/install.sh && rm -rf $CPPCHECK_DIR/.git/
ENV SKIP_CPPCHECK_INSTALL=1

# TODO: this should be a "pip install" or not even in this repo at all
ENV OPENDBC_REF="74e042d4e76651d21b48db2c87c092d8855e9bdc"
RUN git config --global --add safe.directory /tmp/openpilot/panda
RUN mkdir -p /tmp/openpilot/ && \
    cd /tmp/openpilot/ && \
    git clone --depth 1 https://github.com/commaai/opendbc && \
    cd opendbc && git fetch origin $OPENDBC_REF && git checkout FETCH_HEAD && rm -rf .git/ && \
    pip3 install --break-system-packages --no-cache-dir . && \
    scons -j8 --minimal opendbc/

# for Jenkins
COPY README.md panda.tar.* /tmp/
RUN mkdir /tmp/openpilot/panda && \
    tar -xvf /tmp/panda.tar.gz -C /tmp/openpilot/panda/ || true
