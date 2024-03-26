FROM debian:unstable-slim
RUN apt update && apt install -y python3-pyelftools python3-requests git cmake ninja-build \
    build-essential pkg-config libicu-dev libcapstone-dev
COPY . /blutter
VOLUME [ "/data/lib", "/data/output" ]
WORKDIR /blutter
CMD [ "python3", "blutter.py",  "/data/lib", "/data/output"]