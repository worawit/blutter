# Use Ubuntu 23.10 as the base image
FROM ubuntu:23.10

# Update and upgrade the system
RUN apt-get update && \
    apt-get upgrade -y

# Install necessary packages
RUN apt-get install -y \
    python3-pyelftools \
    python3-requests \
    git \
    cmake \
    ninja-build \
    build-essential \
    pkg-config \
    libicu-dev \
    libcapstone-dev

# Clone the specified repository
RUN git clone https://github.com/worawit/blutter.git

# Set the working directory to the cloned repository
WORKDIR /blutter

# Entry point for running the specific command
ENTRYPOINT ["python3", "blutter.py"]

# Default command arguments (can be overridden when running the container)
CMD ["/app/arm64-v8a", "/app/blutter_output"]
