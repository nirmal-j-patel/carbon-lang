# Part of the Carbon Language project, under the Apache License v2.0 with LLVM
# Exceptions. See /LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

FROM ubuntu:24.04 as carbon-ubuntu2404-base

# Install apt tools:
#   git: Used by VS Code.
#   golang: Used for Bazelisk and Buildifier.
#   python3: For Carbon tools.
#   gnupg, software-properties-common, wget: For llvm.sh.
# apt-get update and install together per Docker best practice.
RUN apt-get update && \
  apt-get install -y \
  git \
  gnupg \
  golang \
  python3-pip \
  python3 \
  software-properties-common \
  wget \
  nodejs \
  npm

ENV PATH="/root/go/bin:${PATH}"

# Bazelisk is used for Carbon builds.
RUN go install github.com/bazelbuild/bazelisk@v1.14.0
RUN ln -s /root/go/bin/bazelisk /root/go/bin/bazel

# Buildifier is used by the Bazel VS Code extension.
RUN go install github.com/bazelbuild/buildtools/buildifier@5.1.0

# Install LLVM from apt.llvm.org.
RUN wget https://apt.llvm.org/llvm.sh
RUN chmod +x llvm.sh
RUN ./llvm.sh 18 all
RUN rm llvm.sh

# Add the lib dir to the PATH. This helps Bazel find clang and VS Code find
# clangd, without version suffixes.
ENV PATH="/usr/lib/llvm-18/bin:${PATH}"

# Update pip and install black and pre-commit.
RUN pip3 install black pre-commit --break-system-packages

# Create .cache directory with proper ownership. We will mount a named volume
# at this location to allow caching of bazel build files.
RUN mkdir /home/ubuntu/.cache && chown -R ubuntu:ubuntu /home/ubuntu/.cache
