<h1 align="center">COSMOS</h1>

<p align="center">
  <strong>RL-Enhanced Locality-Aware Counter Cache Optimization for Secure Memory</strong>
</p>

<p align="center">
  <a href="https://doi.org/10.1145/3725843.3756047"><img alt="MICRO 2025" src="https://img.shields.io/badge/paper-MICRO%202025-8A2BE2"></a>
  <img alt="gem5 v24.0.0.0" src="https://img.shields.io/badge/gem5-v24.0.0.0-4C9A2A">
  <img alt="BSD 3-Clause" src="https://img.shields.io/badge/license-BSD--3--Clause-D66A36">
</p>

<!-- Add your cosmic illustration to doc/cosmos_banner.png to display it here. -->
<p align="center">
  <img src="doc/cosmos_banner.png" alt="COSMOS: cosmic-themed project illustration" width="420">
</p>

<p align="center">
  <a href="https://doi.org/10.1145/3725843.3756047">Paper</a>
  ·
  <a href="https://github.com/gem5/gem5">gem5</a>
  ·
  <a href="#installation">Installation</a>
  ·
  <a href="#citation">Citation</a>
</p>

---

## Overview

**COSMOS** (**C**ounter **O**ptimized **S**ecure **M**emory **O**peration **S**cheme) is a secure-memory architecture that uses reinforcement learning (RL) to reduce the performance overhead of counter-based memory protection. It combines two mechanisms:

- **Data-location prediction (DP):** predicts whether requested data is on-chip or off-chip following an L1 data-cache miss, allowing early counter access when an off-chip access is predicted.
- **Counter-locality prediction (CP):** predicts the locality of counter accesses and guides a locality-centric replacement policy for the **LCR-CTR cache**.

This repository releases the **gem5 simulator implementation** of COSMOS described in our [MICRO 2025 paper](https://doi.org/10.1145/3725843.3756047). The implementation extends the gem5 memory controller with COSMOS prediction, metadata-traffic, and timing models.


## Requirements

- **gem5 v24.0.0.0**. This is the target gem5 release; compatibility with other revisions has not been verified.
- A C++ toolchain and the build dependencies required by gem5 24.0.
- An existing gem5 simulation configuration and workload suitable for your research.

See the [gem5 repository](https://github.com/gem5/gem5) and [gem5 documentation](https://www.gem5.org/documentation/) for general installation and simulation guidance.

## Repository Structure

```text
COSMOS/
├── README.md
├── LICENSE
├── doc/
│   └── cosmos_banner.png    # Optional README illustration
├── src/
│   └── mem/
│       ├── cosmos_rl.hh      # RL predictors, Q-tables, CET, LCR-CTR cache
│       ├── mem_ctrl.cc       # COSMOS-enabled memory-controller implementation
│       ├── mem_ctrl.hh       # Memory-controller declarations and statistics
│       └── MemCtrl.py        # gem5 SimObject parameters for COSMOS
└── patches/
    └── packet.hh.patch      # Metadata-packet marker in gem5 Packet
```

**Integration summary:** `mem_ctrl.cc`, `mem_ctrl.hh`, and `MemCtrl.py` **replace** the corresponding files in `gem5/src/mem/`; `cosmos_rl.hh` is an **additional** header in that directory. The patch modifies `gem5/src/mem/packet.hh`. The released files implement the COSMOS memory-controller extensions and RL features.

## Installation

The steps below assume a **clean gem5 checkout** and that the COSMOS repository is available locally. All commands after entering the gem5 directory are run from the gem5 root.

### 1. Obtain gem5 24.0

```bash
git clone https://github.com/gem5/gem5.git
cd gem5
git checkout v24.0.0.0
```

### 2. Copy the COSMOS implementation

Set `COSMOS_ROOT` to the absolute path of your COSMOS repository:

```bash
export COSMOS_ROOT=/absolute/path/to/COSMOS
```

**Warning:** The following commands overwrite three upstream gem5 files. Back up any local changes first.

```bash
cp "$COSMOS_ROOT/src/mem/cosmos_rl.hh" src/mem/
cp "$COSMOS_ROOT/src/mem/mem_ctrl.cc"  src/mem/
cp "$COSMOS_ROOT/src/mem/mem_ctrl.hh"  src/mem/
cp "$COSMOS_ROOT/src/mem/MemCtrl.py"   src/mem/
```

### 3. Apply the Packet extension

COSMOS injects simulated security-metadata requests at the memory controller. The patch adds a `bespoke` marker to gem5's `Packet` class so these injected packets can be distinguished from regular requests.

```bash
git apply --check "$COSMOS_ROOT/patches/packet.hh.patch"
git apply "$COSMOS_ROOT/patches/packet.hh.patch"
```

If the patch does not apply, confirm the gem5 revision and ensure `src/mem/packet.hh` has not already been modified. Do not apply the same patch twice.

### 4. Build gem5

For example, to compile an optimized X86 simulator:

```bash
scons build/X86/gem5.opt -j"$(nproc)"
```

Choose a different build target if your simulation uses another supported ISA. Compiling the modified simulator **does not automatically enable COSMOS**: the memory controller must also be configured as described below.

## Using COSMOS

### Enable COSMOS in your gem5 configuration

COSMOS exposes its controls through the modified `MemCtrl` SimObject. Set these parameters on each relevant `MemCtrl` instance **before calling `m5.instantiate()`**.

The following is an **illustrative configuration snippet**, not a standalone runnable gem5 script. `ctrl`, `l1d_cache`, `l2_cache`, and `llc_cache` must refer to objects constructed by your own configuration:

```python
# ctrl: an existing gem5 MemCtrl instance connected to DRAM
# l1d_cache, l2_cache, llc_cache: existing cache objects

# Enable the COSMOS memory model and both predictors.
ctrl.cosmos = True
ctrl.cosmos_dp = True
ctrl.cosmos_cp = True

# Wire cache probe sources for data-location prediction.
ctrl.cosmos_l1d = [l1d_cache]
ctrl.cosmos_l2 = [l2_cache]
ctrl.cosmos_llc = [llc_cache]

# Example COSMOS parameter settings (also the Python defaults).
ctrl.cosmos_lcr_blocks = 2048
ctrl.cosmos_lcr_assoc = 16
ctrl.cosmos_meta_blocks = 2048
ctrl.cosmos_vn_per_block = 64
ctrl.cosmos_tree_arity = 8
ctrl.cosmos_seed = 1
```

In multicore systems, pass the applicable cache objects in the probe-source lists, and configure memory controllers according to your hierarchy. In particular:

- `cosmos_l1d` supplies **L1 data-cache miss** probes to the data-location predictor.
- `cosmos_l2` and `cosmos_llc` supply hit/miss events used to resolve whether data remained on-chip.
- If `cosmos_dp=True` but `cosmos_l1d` is empty, the implementation warns that the data-location predictor will be inactive.

### Main Configuration Parameters

The following parameters are declared in `src/mem/MemCtrl.py`:

| Parameter | Default | Description |
| --- | --- | --- |
| `cosmos` | `False` | Enable COSMOS secure-memory modeling |
| `cosmos_dp` | `True` | Enable data-location prediction and early counter access |
| `cosmos_cp` | `True` | Enable counter-locality prediction and LCR replacement; `False` selects the LRU counter-cache policy |
| `cosmos_l1d` | `[]` | L1 data caches whose misses trigger data-location prediction |
| `cosmos_l2` | `[]` | L2 caches used to resolve on-chip accesses |
| `cosmos_llc` | `[]` | Last-level caches used to resolve data location |
| `cosmos_lcr_blocks` | `2048` | Counter-cache capacity, in 64-byte blocks |
| `cosmos_lcr_assoc` | `16` | Counter-cache associativity |
| `cosmos_meta_blocks` | `2048` | Metadata-cache capacity, in 64-byte blocks |
| `cosmos_vn_per_block` | `64` | Minor counters per 64-byte counter block |
| `cosmos_tree_arity` | `8` | Integrity-tree arity |
| `cosmos_aes_latency` | `13.3ns` | Modeled AES latency |
| `cosmos_auth_latency` | `13.3ns` | Modeled authentication latency |
| `cosmos_meta_step` | `45ns` | Modeled serial delay per counter-miss level |
| `cosmos_seed` | `1` | Seed for RL exploration |

### Tuning the RL Configuration

The RL algorithms and their compile-time parameters are defined in `src/mem/cosmos_rl.hh`, including:

- Q-table state count and learning parameters (`ALPHA_*`, `GAMMA_*`, `EPS_*`)
- Reward values for both predictors (`R_D_*` and `R_C_*`)
- CET size (`CET_ENTRIES`) and nearby-address window (`CET_WINDOW`)
- Counter-cache management and prediction-scoring behavior

These values are C++ constants rather than gem5 Python parameters. **Rebuild gem5 after changing them**. If you use different workloads or cache configurations, consider tuning the capacity and RL parameters for your environment.

## Running Simulations and Inspecting Statistics

Use your normal gem5 configuration and workloads with the COSMOS-enabled `MemCtrl`. For example, the invocation structure is:

```bash
build/X86/gem5.opt --outdir=m5out /path/to/your_config.py [config options]
```

`/path/to/your_config.py` and `[config options]` are **placeholders** for your own simulation setup; a complete reference experiment configuration is not included in this release.

COSMOS records statistics through gem5's standard statistics system, usually available in `m5out/stats.txt`. Representative names include:

| Statistic | Meaning |
| --- | --- |
| `cosDpAccuracy` | Data-location predictor accuracy |
| `cosCpGoodFrac` | Fraction of counter-locality decisions predicting good locality |
| `cosCetHits`, `cosCetMisses`, `cosCetEvicts` | CET lookup and eviction activity |
| `cosLcrHits`, `cosLcrMisses`, `cosLcrMissRate` | LCR-CTR cache behavior |
| `cosLcrEarlyAccesses`, `cosLcrDemandAccesses` | Counter accesses initiated early versus at demand time |
| `secVnReads`, `secMacReads`, `secMtReads` | Modeled security-metadata reads |

These statistics describe **different quantities**. In particular, `cosCpGoodFrac` is **not** the CET hit rate or the counter-cache hit rate. Use the same workload, hierarchy, and parameter definitions when comparing experiments.


## Paper and Citation

**Haoran Geng, Xiaoyang Lu, Yuezhi Che, Ziang Tian, Dazhao Cheng, Xian-He Sun, Michael Niemier, and X. Sharon Hu.**  
**“COSMOS: RL-Enhanced Locality-Aware Counter Cache Optimization for Secure Memory.”**  
*Proceedings of the 58th IEEE/ACM International Symposium on Microarchitecture (MICRO '25)*, 2025, pp. 1073–1086.  
**DOI:** [10.1145/3725843.3756047](https://doi.org/10.1145/3725843.3756047)

If you use COSMOS in your research, please cite the paper:

```bibtex
@inproceedings{geng2025cosmos,
  author    = {Haoran Geng and Xiaoyang Lu and Yuezhi Che and
               Ziang Tian and Dazhao Cheng and Xian-He Sun and
               Michael T. Niemier and X. Sharon Hu},
  title     = {{COSMOS}: {RL}-Enhanced Locality-Aware Counter Cache
               Optimization for Secure Memory},
  booktitle = {Proceedings of the 58th IEEE/ACM International Symposium
               on Microarchitecture (MICRO '25)},
  year      = {2025},
  pages     = {1073--1086},
  doi       = {10.1145/3725843.3756047}
}
```

## License

The original COSMOS contributions are intended to be distributed under the **BSD 3-Clause License**. See [`LICENSE`](LICENSE) for the license text and applicable copyright holder(s).

**Upstream code notice:** Some files in this repository are modified versions of gem5 source files and retain their original copyright statements and license notices. Those existing notices and license obligations continue to apply. The COSMOS license does not supersede upstream or third-party rights.

## Scope and Limitations

- This repository includes the COSMOS **simulator implementation**, including RL prediction and associated memory-controller extensions.
- It **does not** distribute the full benchmark suite, original testing configurations, raw measurement data, or figure-generation scripts.
- Results may vary with different versions of gem5, CPU/cache/memory configurations, benchmarks, or RL settings.
- The implementation is a **research timing and traffic model**, not a production secure-memory cryptographic implementation.

## Questions

For questions or bug reports about the released code, please open a GitHub Issue. For details of the research methodology, please consult the [MICRO 2025 paper](https://doi.org/10.1145/3725843.3756047).
