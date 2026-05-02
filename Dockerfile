FROM ghcr.io/astral-sh/uv:python3.12-bookworm

# Add the Debian testing repository to get GCC 13
RUN echo "deb http://deb.debian.org/debian testing main" >> /etc/apt/sources.list && \
    apt-get update && \
    apt-get install -y gcc-13 \
    g++-13 \
    git \
    cmake \
    ninja-build \
    build-essential \
    pkg-config \
    libicu-dev \
    libcapstone-dev && \
    apt-get clean && \
    uv pip install --system pyelftools requests && \
    # Set GCC 13 as the default compiler
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 60 \
    --slave /usr/bin/g++ g++ /usr/bin/g++-13

COPY . /app
WORKDIR /app