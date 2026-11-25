#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" = scanner ]; then
 # Second target: firmware offset scanner (diagnostics/fw_offset_scanner.c).
 SCANNER_OUTPUT=${SCANNER_OUTPUT:-build/fw-offset-scanner.elf}
 mkdir -p "$(dirname "$SCANNER_OUTPUT")"
 "${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}/bin/prospero-clang" -O2 -Wall -Wextra -Werror diagnostics/fw_offset_scanner.c -o "$SCANNER_OUTPUT" -lkernel_sys
 sha256sum "$SCANNER_OUTPUT"
 exit 0
fi
NEO_DIAGNOSTIC_OUTPUT=${NEO_DIAGNOSTIC_OUTPUT:-build/neo-diagnostic.elf}
mkdir -p "$(dirname "$NEO_DIAGNOSTIC_OUTPUT")"
"${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}/bin/prospero-clang" -O2 -Wall -Wextra -Werror diagnostics/neo_diagnostic.c -o "$NEO_DIAGNOSTIC_OUTPUT" -lkernel_sys
sha256sum "$NEO_DIAGNOSTIC_OUTPUT"
