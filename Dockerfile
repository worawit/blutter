FROM ghcr.io/astral-sh/uv:debian-slim
COPY --from=ghcr.io/astral-sh/uv:latest /uv /uvx /bin/

RUN apt-get update && apt-get install -y  python3-pyelftools python3-requests git cmake ninja-build \
    build-essential pkg-config libicu-dev libcapstone-dev

# Add the Debian testing repository to get GCC 13
RUN echo "deb http://deb.debian.org/debian testing main" >> /etc/apt/sources.list
RUN apt-get update && apt-get install -y gcc-13 g++-13

# Set GCC 13 as the default compiler
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 60 \
    --slave /usr/bin/g++ g++ /usr/bin/g++-13

COPY . /app
WORKDIR /app