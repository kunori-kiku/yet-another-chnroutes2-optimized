# Yet another chnroutes2 optimized

This repository publishes **China IP route sets** in two formats:

* **Plain text CIDR lists** (easy to consume by scripts and other tools)
* **nftables `set` definitions** (`flags interval`, designed to be included inside an existing `table inet ... { }`)

All generated outputs are normalized into a **non-overlapping interval set** representation (no overlaps/containment in interval semantics), suitable for nftables interval sets.

---

## Files

### Text outputs

* **`ip4.txt`** - Aggregated IPv4 CIDR list (non-overlapping in interval semantics)
* **`ip6.txt`** - Aggregated IPv6 CIDR list (non-overlapping in interval semantics)

Each file includes a small header indicating the upstream source URL and generation time.

### nftables outputs

* **`cn4.nft`** - `set cn4 { ... }` for IPv4 (`type ipv4_addr; flags interval;`)
* **`cn6.nft`** - `set cn6 { ... }` for IPv6 (`type ipv6_addr; flags interval;`)

These files **only contain `set` definitions** to maximize flexibility. They are meant to be included inside your own nftables table, for example:

```nft
table inet my_table {
  include "cn4.nft"
  include "cn6.nft"
}
```

---

## Sync Script

A helper script `sync-cn-nft.sh` is provided to **periodically fetch nftables sets from GitHub Releases** and update local files safely.

### Features

* Fetch **IPv4**, **IPv6**, or **both**
* Supports **GitHub mirrors** (e.g. `gh-proxy.com`)
* Verifies:

  * the fetched `.nft` file is **not empty**
  * nftables **syntax check passes** (`nft -c`)
* Uses **atomic replacement** to avoid partial updates

### Example

```
./sync-cn-nft.sh \
  --dest /etc/nftables.d \
  --mode both
```

Using a GitHub download mirror:

```
./sync-cn-nft.sh \
  --dest /etc/nftables.d \
  --mode both \
  --mirror-prefix "https://gh-proxy.com/"
```

Further help see when running the script with `--help`:

```
./sync-cn-nft.sh --help
```

---

## Upstream Sources

* **IPv4**: [`chnroutes2-optimized`](https://github.com/SukkaW/chnroutes2-optimized) by SukkaW
* **IPv6**: [`Sukka Ruleset`](https://github.com/SukkaW/Surge) by SukkaW

The generated outputs embed the **upstream URL** in their headers for traceability.

---

## License

This repository follows MIT License for its code.

Any content redistributed and processed by this repository is subject to the **license(s) of the upstream data sources**.
Please refer to the upstream projects for their specific licensing terms.

By using the generated outputs, you agree to comply with the respective upstream licenses.

