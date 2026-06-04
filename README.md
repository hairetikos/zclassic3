Zclassic 3.0 — "sigma"
substantially.

Network facts (mainnet)
-----------------------

| Item | Value |
| --- | --- |
| Currency unit | `ZCL` |
| Default P2P port | `8033` |
| Default RPC port | `8232` |
| Transparent address prefix | `t1…` / `t3…` |
| Sapling shielded address prefix | `zs…` |
| BIP44 coin type | `147` |

#### :lock: Security Warnings

**Zclassic 3.0 is experimental and a work in progress.** Use it at your own risk.
Tor Onion v3 support is *initial* — exercise it on testnet and verify behavior
before relying on it for strong anonymity. Always keep encrypted backups of your
wallet.

Getting Started
---------------

### Building

Build `zclassicd` along with most of its dependencies from source:

```
./zcutil/build.sh -j$(nproc)
```

Useful build options:

```
./zcutil/build.sh -rebuild -j$(nproc)        # fast incremental rebuild after edits
MARCH_NATIVE=1 ./zcutil/build.sh -j$(nproc)  # CPU-tuned binary (recommended)
OPTIMIZE=1     ./zcutil/build.sh -j$(nproc)  # -O3 -march=native
```

Zclassic is officially supported on Debian and Ubuntu.

### Running

```
./src/zclassicd                      # start the node
./src/zclassic-cli getinfo           # query a running node
./src/zclassic-cli help              # list RPC commands
```

To run as a Tor v3 hidden service, run a local Tor daemon with its control port
enabled and start `zclassicd` with `-listenonion=1` (the default proxy/control
settings match a standard Tor install). The node will create a persistent v3
`.onion` service and advertise it to v3-capable peers.

License
-------

For license information see the file [COPYING](COPYING).

Zclassic builds on the work of the Zcash and Bitcoin Core developers; see
[`doc/authors.md`](doc/authors.md).
