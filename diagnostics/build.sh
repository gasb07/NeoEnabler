#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
NEO_DIAGNOSTIC_OUTPUT=${NEO_DIAGNOSTIC_OUTPUT:-build/neo-diagnostic.elf}
mkdir -p "$(dirname "$NEO_DIAGNOSTIC_OUTPUT")"
"${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}/bin/prospero-clang" -O2 -Wall -Wextra -Werror diagnostics/neo_diagnostic.c -o "$NEO_DIAGNOSTIC_OUTPUT" -lkernel_sys
sha256sum "$NEO_DIAGNOSTIC_OUTPUT"
