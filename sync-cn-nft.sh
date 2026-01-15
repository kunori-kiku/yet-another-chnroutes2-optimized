#!/bin/sh
set -eu

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

# --- arg parsing (POSIX) ---
while [ $# -gt 0 ]; do
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

if [ -z "$DEST" ] || [ -z "$MODE" ]; then
  echo "ERROR: --dest, --mode are required." >&2
  usage
  exit 2
fi

if [ "$MODE" != "v4" ] && [ "$MODE" != "v6" ] && [ "$MODE" != "both" ]; then
  echo "ERROR: --mode must be v4|v6|both" >&2
  exit 2
fi

mkdir -p "$DEST"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "ERROR: missing command: $1" >&2; exit 1; }
}

need_cmd curl
if [ "$NO_NFT_CHECK" -eq 0 ]; then
  need_cmd nft
fi

# Normalize MIRROR_PREFIX: allow "https://gh-proxy.com" or "https://gh-proxy.com/"
if [ -n "$MIRROR_PREFIX" ]; then
  case "$MIRROR_PREFIX" in
    */) : ;;
    *) MIRROR_PREFIX="${MIRROR_PREFIX}/" ;;
  esac
fi

origin_base="https://github.com/${REPO}/releases/download/${TAG}"

# Remove trailing slashes from BASE_URL (POSIX safe)
trim_trailing_slash() {
  # prints trimmed version of $1
  echo "$1" | sed 's:/*$::'
}

build_url() {
  asset="$1"
  if [ -n "$BASE_URL" ]; then
    b="$(trim_trailing_slash "$BASE_URL")"
    echo "${b}/${asset}"
  else
    u="${origin_base}/${asset}"
    if [ -n "$MIRROR_PREFIX" ]; then
      echo "${MIRROR_PREFIX}${u}"
    else
      echo "${u}"
    fi
  fi
}

is_non_empty_nft() {
  file="$1"
  # must have "set NAME {"
  grep -Eq '^[[:space:]]*set[[:space:]]+[A-Za-z0-9_]+[[:space:]]*\{' "$file" || return 1
  # must contain at least one CIDR-like token (rough heuristic)
  grep -Eq '([0-9]{1,3}\.){3}[0-9]{1,3}/[0-9]{1,2}|:[0-9A-Fa-f]*::?[0-9A-Fa-f:]*/[0-9]{1,3}' "$file" || return 1
  return 0
}

nft_check_set_only() {
  nftfile="$1"
  tmpdir="$2"
  wrapper="${tmpdir}/wrapper.nft"

  # Resolve to an absolute path where possible, to avoid ambiguous includes.
  nftfile_abs="$nftfile"
  if command -v realpath >/dev/null 2>&1; then
    rp="$(realpath "$nftfile" 2>/dev/null || true)"
    if [ -n "$rp" ]; then
      nftfile_abs="$rp"
    fi
  fi

  # Basic validation: disallow characters that could break the quoted string.
  case "$nftfile_abs" in
    *\"*|*'
'*)
      echo "[ERROR] Invalid nft file path for include: ${nftfile_abs}" >&2
      return 1
      ;;
  esac

  cat >"$wrapper" <<EOF
table inet test_sync {
  include "${nftfile_abs}"
}
EOF

  nft -c -f "$wrapper" >/dev/null 2>&1
}

fetch_one() {
  asset="$1"
  local_name="$2"
  url="$(build_url "$asset")"

  tmpdir="$(mktemp -d)"
  tmpfile="${tmpdir}/${local_name}.new"

  # Ensure cleanup on any return from this function call path
  # (POSIX sh: no RETURN trap, so explicit cleanup is safest)
  cleanup() { rm -rf "$tmpdir"; }
  # shellcheck disable=SC2064
  trap 'cleanup' INT TERM HUP EXIT

  echo "[INFO] Fetching: ${url}"
  if ! curl -LfsS --connect-timeout "$TIMEOUT" --max-time "$TIMEOUT" "$url" -o "$tmpfile"; then
    echo "[ERROR] Download failed: ${url}" >&2
    cleanup
    trap - INT TERM HUP EXIT
    return 1
  fi

  if ! is_non_empty_nft "$tmpfile"; then
    echo "[ERROR] Fetched file looks empty or malformed: ${asset}" >&2
    echo "        Refusing to overwrite local file." >&2
    cleanup
    trap - INT TERM HUP EXIT
    return 1
  fi

  if [ "$NO_NFT_CHECK" -eq 0 ]; then
    if ! nft_check_set_only "$tmpfile" "$tmpdir"; then
      echo "[ERROR] nft syntax check failed for: ${asset}" >&2
      echo "        Refusing to overwrite local file." >&2
      cleanup
      trap - INT TERM HUP EXIT
      return 1
    fi
  fi

  dest_path="${DEST}/${local_name}"
  bak_path="${DEST}/${local_name}.bak"

  if [ -f "$dest_path" ]; then
    cp -f "$dest_path" "$bak_path" >/dev/null 2>&1 || true
  fi

  mv -f "$tmpfile" "$dest_path"

  echo "[OK] Updated: ${dest_path}"

  cleanup
  trap - INT TERM HUP EXIT
  return 0
}

case "$MODE" in
  v4)
    fetch_one "cn4.nft" "cn4.nft"
    ;;
  v6)
    fetch_one "cn6.nft" "cn6.nft"
    ;;
  both)
    fetch_one "cn4.nft" "cn4.nft"
    fetch_one "cn6.nft" "cn6.nft"
    ;;
esac
