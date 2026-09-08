#!/bin/bash
set -euo pipefail
CX=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver
for d in "$CX/lib/dxmt/x86_64-windows" "$CX/lib/dxmt/x86_64-unix"; do
  for b in "$d"/*.stock; do [ -f "$b" ] && mv "$b" "${b%.stock}" && echo "restored ${b%.stock}"; done
done
