ARG DOCKER_TAG=v5.5.1
FROM espressif/idf:${DOCKER_TAG}

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

# The upstream image owns ESP-IDF as root, while the devcontainer runs as the
# ordinary ubuntu user. Mark only the bundled ESP-IDF tree and its nested Git
# worktrees safe for ESP-IDF's version checks.
RUN find /opt/esp/idf -name .git -print \
    | sed 's#/.git$##' \
    | while read -r repo; do git config --system --add safe.directory "$repo"; done

# Build as the ordinary user already provided by the upstream image so a Linux
# host is not left with root-owned build artifacts. The upstream image already
# contains ESP-IDF, CMake, Ninja, Python, and the Xtensa toolchain and activates
# them from its entrypoint.
ENV HOME=/home/ubuntu
USER ubuntu
