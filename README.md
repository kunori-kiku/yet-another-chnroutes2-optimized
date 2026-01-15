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

These files **only contain `set` definitions**. They are meant to be included inside your own nftables table, for example:

```nft
table inet my_table {
  include "cn4.nft"
  include "cn6.nft"
}
```

---

## Upstream Sources

* **IPv4**: `chnroutes2-optimized` by SukkaW
* **IPv6**: configurable upstream source (URL recorded in the generated file headers)

The generated outputs embed the **upstream URL** in their headers for traceability.

---

## License

This repository follows MIT License for its code.
Any content redistributed and processed by this repository is subject to the **license(s) of the upstream data sources**.
Please refer to the upstream projects for their specific licensing terms.
