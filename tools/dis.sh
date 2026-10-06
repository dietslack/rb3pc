#!/usr/bin/env bash
# Disassemble a range of a dumped .text section.
# Usage: tools/dis.sh <image_dir> <start_hex> <end_hex>
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
objdump="$here/reference/rexglue-sdk/tools/binutils/powerpc-none-elf-objdump"
read -r _ vma _ _ < <(grep '^\.text ' "$1/sections.txt")
"$objdump" -D -b binary -m powerpc:common64 -EB -M altivec,64 \
  --adjust-vma="$vma" --start-address="$2" --stop-address="$3" "$1/text.bin" |
  sed -n '/>:$/,$p' | tail -n +2
