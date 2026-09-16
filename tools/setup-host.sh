#!/bin/sh
# One-time host setup: submodules + Python venv for the nanopb generator.
# bell's nanopb is old: it needs the protobuf 4.x API and pkg_resources.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
git submodule update --init --recursive
python3 -m venv .venv
.venv/bin/pip install -q "protobuf==4.25.3" "grpcio-tools==1.62.3" "setuptools<70"
find third_party/cspot/cspot/bell/external/nanopb -name nanopb_pb2.py -delete
echo "host ready"
