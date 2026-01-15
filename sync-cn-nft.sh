#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  sync-cn-nft.sh --dest DIR --mode v4|v6|both [--repo OWNER/REPO] [--tag TAG]
                 [--base-url URL] [--mirror-prefix URL] [--no-nft-check] [--timeout SEC]

Download URL rules:
  - If --base-url is provided, assets are fetched from:
      <base-url>/<asset>
  - Else, the default origin base is:
      https://github.com/<repo>/releases/download/<tag>
    and if --mirror-prefix is provided, it becomes:
      <mirror-prefix>https://github.com/<repo>/releases/download/<tag>/<asset>
    (Example mirror: https://gh-proxy.com/)
  - By default, the repo is kunori-kiku/yet-another-chnroutes2-optimized
    and the tag is cnroutes-latest.

Examples:
  # Standard GitHub
  sync-cn-nft.sh --repo usrname/yourrepo --tag cnroutes-latest --dest /etc/nftables.d --mode both

  # Using gh-proxy.com as a prefix mirror
  sync-cn-nft.sh --repo usrname/yourrepo --tag cnroutes-latest --dest /etc/nftables.d --mode both \
    --mirror-prefix "https://gh-proxy.com/"

  # Fully custom base URL (must directly host cn4.nft/cn6.nft)
  sync-cn-nft.sh --repo usrname/yourrepo --tag cnroutes-latest --dest /etc/nftables.d --mode both \
    --base-url "https://my-cdn.example.com/cnroutes-latest"
EOF
}

REPO="kunori-kiku/yet-another-chnroutes2-optimized"
TAG="cnroutes-latest"
DEST=""
MODE=""
BASE_URL=""
MIRROR_PREFIX=""
NO_NFT_CHECK=0
TIMEOUT=30

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) REPO="$2"; shift 2;;
    --tag) TAG="$2"; shift 2;;
    --dest) DEST="$2"; shift 2;;
    --mode) MODE="$2"; shift 2;;
    --base-url) BASE_URL="$2"; shift 2;;
    --mirror-prefix) MIRROR_PREFIX="$2"; shift 2;;
    --no-nft-check) NO_NFT_CHECK=1; shift 1;;
    --timeout) TIMEOUT="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2;;
  esac
done


if [[ -z "$DEST" || -z "$MODE" ]]; then
  echo "ERROR: --dest, --mode are required." >&2
  usage
  exit 2
fi

if [[ "$MODE" != "v4" && "$MODE" != "v6" && "$MODE" != "both" ]]; then
  echo "ERROR: --mode must be v4|v6|both" >&2
  exit 2
fi

mkdir -p "$DEST"

need_cmd() { command -v "$1" >/dev/null 2>&1 || { echo "ERROR: missing command: $1" >&2; exit 1; }; }
need_cmd curl
if [[ "$NO_NFT_CHECK" -eq 0 ]]; then
  need_cmd nft
fi

# Normalize MIRROR_PREFIX: allow "https://gh-proxy.com" or "https://gh-proxy.com/"
if [[ -n "$MIRROR_PREFIX" && "${MIRROR_PREFIX: -1}" != "/" ]]; then
  MIRROR_PREFIX="${MIRROR_PREFIX}/"
fi

origin_base="https://github.com/${REPO}/releases/download/${TAG}"

build_url() {
  local asset="$1"
  if [[ -n "$BASE_URL" ]]; then
    # base-url is a direct base hosting assets
    echo "${BASE_URL%/}/${asset}"
  else
    local u="${origin_base}/${asset}"
    if [[ -n "$MIRROR_PREFIX" ]]; then
      echo "${MIRROR_PREFIX}${u}"
    else
      echo "${u}"
    fi
  fi
}

is_non_empty_nft() {
  local file="$1"
  grep -Eq '^\s*set\s+[A-Za-z0-9_]+\s*\{' "$file" || return 1
  grep -Eq '([0-9]{1,3}\.){3}[0-9]{1,3}/[0-9]{1,2}|:[0-9A-Fa-f]*::?[0-9A-Fa-f:]*/[0-9]{1,3}' "$file" || return 1
  return 0
}

nft_check_set_only() {
  local nftfile="$1"
  local tmpdir="$2"
  local wrapper="${tmpdir}/wrapper.nft"
  cat >"$wrapper" <<EOF
table inet test_sync {
  include "${nftfile}"
}
EOF
  nft -c -f "$wrapper" >/dev/null 2>&1
}

fetch_one() {
  local asset="$1"
  local local_name="$2"
  local url
  url="$(build_url "$asset")"

  local tmpdir
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' RETURN

  local tmpfile="${tmpdir}/${local_name}.new"

  echo "[INFO] Fetching: ${url}"
  curl -LfsS --connect-timeout "$TIMEOUT" --max-time "$TIMEOUT" "$url" -o "$tmpfile"

  if ! is_non_empty_nft "$tmpfile"; then
    echo "[ERROR] Fetched file looks empty or malformed: ${asset}" >&2
    echo "        Refusing to overwrite local file." >&2
    return 1
  fi

  if [[ "$NO_NFT_CHECK" -eq 0 ]]; then
    if ! nft_check_set_only "$tmpfile" "$tmpdir"; then
      echo "[ERROR] nft syntax check failed for: ${asset}" >&2
      echo "        Refusing to overwrite local file." >&2
      return 1
    fi
  fi

  local dest_path="${DEST}/${local_name}"
  local bak_path="${DEST}/${local_name}.bak"

  [[ -f "$dest_path" ]] && cp -f "$dest_path" "$bak_path" || true
  mv -f "$tmpfile" "$dest_path"

  echo "[OK] Updated: ${dest_path}"
}

case "$MODE" in
  v4)   fetch_one "cn4.nft" "cn4.nft" ;;
  v6)   fetch_one "cn6.nft" "cn6.nft" ;;
  both) fetch_one "cn4.nft" "cn4.nft"
        fetch_one "cn6.nft" "cn6.nft" ;;
esac
