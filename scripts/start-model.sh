#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
model_file="${MODEL_FILE:-$project_dir/../Qwen3.5-4B-GGUF/Qwen3.5-4B-Q8_0.gguf}"
server_bin="${LLAMA_SERVER_BIN:-llama-server}"
[[ -f "$model_file" ]] || { printf '找不到模型：%s\n' "$model_file" >&2; exit 1; }
command -v "$server_bin" >/dev/null || { printf '找不到 llama-server。\n' >&2; exit 1; }
device_args=(--device none --n-gpu-layers 0)
if [[ "$(uname -s)" == Darwin ]]; then
  device_args=(--device MTL0 --n-gpu-layers all)
fi
exec "$server_bin" --model "$model_file" --alias qwen3.5-4b \
  --host 127.0.0.1 --port "${MODEL_PORT:-8080}" --cors-origins localhost \
  "${device_args[@]}" --ctx-size "${MODEL_CTX:-65536}" --parallel 2 --no-kv-unified \
  --flash-attn on --cache-type-k f16 --cache-type-v f16 \
  --jinja --reasoning off --no-context-shift --slots \
  --temp 0.2 --top-p 0.8 --top-k 20 --min-p 0 --repeat-penalty 1 \
  --presence-penalty 0 "$@"
