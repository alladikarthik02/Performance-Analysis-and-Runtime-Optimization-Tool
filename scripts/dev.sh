#!/usr/bin/env bash
# Run any command inside the hotpath container, with the repo bind-mounted.
#
#   ./scripts/dev.sh                 -> interactive shell
#   ./scripts/dev.sh <cmd> [args..]  -> run one command and exit
#
# WHY BIND-MOUNT INSTEAD OF COPY:
#   Edits land on the host (real files, real git history, host editor) while
#   all compilation and execution happen on Linux. Copying into the image
#   would mean a rebuild per edit.
#
# WHY --cap-add SYS_PTRACE / seccomp=unconfined:
#   So gdb works when we need to debug a fault inside a signal handler.
#   This is a local dev container, not a deployed artifact.
set -euo pipefail

# Docker Desktop on macOS registers /usr/local/bin/docker but leaves its
# credential helpers inside the .app bundle, off a non-interactive PATH. The
# CLI reads credsStore=desktop from ~/.docker/config.json and then fails with
# `docker-credential-desktop: executable file not found`. Prepend the bundle
# dir here rather than editing the user's global ~/.docker/config.json --
# scoped to this script, and it changes nothing outside this project.
if [ -d "/Applications/Docker.app/Contents/Resources/bin" ]; then
  PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
fi

IMAGE="hotpath-dev"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo ">>> building $IMAGE (first run only)" >&2
  docker build -t "$IMAGE" "$REPO_ROOT"
fi

exec docker run --rm -it \
  -v "$REPO_ROOT":/work \
  -w /work \
  --cap-add=SYS_PTRACE \
  --security-opt seccomp=unconfined \
  "$IMAGE" \
  "${@:-/bin/bash}"
