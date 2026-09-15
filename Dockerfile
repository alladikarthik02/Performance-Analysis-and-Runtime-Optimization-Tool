# hotpath — reproducible build/profiling environment.
#
# WHY A CONTAINER AT ALL:
#   Profilers are the least portable software there is. This tool depends on
#   SIGPROF delivery semantics, the AArch64 frame-pointer ABI, ELF symbol
#   tables, and PIE load-bias resolution via dl_iterate_phdr. Every one of
#   those is OS-specific. Pinning the OS is not tidiness, it is correctness.
#
# WHY UBUNTU 24.04:
#   glibc 2.39 + GCC 13. We depend on documented glibc behaviour (dladdr,
#   dl_iterate_phdr) and on the AArch64 procedure call standard's frame
#   record layout. A stable, mainstream LTS is the right substrate.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain + the debugging tools we will actually need when this breaks.
#   gdb        - inspecting a corrupted unwind, reading registers at a fault
#   valgrind   - memcheck for the ring buffer / symbolizer
#   binutils   - readelf/nm/objdump to check our ELF parser against ground truth
#   elfutils   - eu-readelf as a second opinion
#   python3    - the analysis + reporting half of the tool
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      gcc \
      g++ \
      cmake \
      ninja-build \
      gdb \
      valgrind \
      binutils \
      elfutils \
      file \
      python3 \
      python3-dev \
      python3-venv \
      git \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# A non-root user would normally be right, but profiling and ptrace-adjacent
# work is simpler as root inside a throwaway container, and nothing here is
# exposed to a network. Documented so it reads as a decision, not an oversight.
CMD ["/bin/bash"]
