# Hyper Uring Server

### High-Performance TCP Server Built on io_uring

::: {align="center"}
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?style=flat-square)]()
[![liburing](https://img.shields.io/badge/liburing-2.2%2B-lightgrey?style=flat-square)]()
[![Kernel](https://img.shields.io/badge/Linux%20Kernel-6.1%2B-brightgreen?style=flat-square)]()
[![Status](https://img.shields.io/badge/Status-Active-success?style=flat-square)]()
:::

------------------------------------------------------------------------

## Overview

**Hyper Uring Server** is a fully asynchronous, zero-copy TCP server
designed to maximize throughput and minimize system overhead using
modern Linux I/O primitives.

The project focuses on:

-   Eliminating syscall overhead\
-   Achieving predictable low latency\
-   Scaling linearly with CPU cores\
-   Providing a minimal and robust architecture for high-load
    environments

> **Note:** The implementation was developed independently by a
> 15-year-old developer, with an emphasis on applying modern
> systems-engineering techniques, io_uring internals, and research-grade
> scalability patterns.

------------------------------------------------------------------------

## Architecture

### Worker-Per-Core Model

Each CPU core runs an isolated process with its own
submission/completion queues.\
No shared memory and no locking ensure deterministic performance under
load.

### Multishot Accept & Multishot Receive

Designed to reduce syscall frequency and improve event handling
efficiency on large connection sets.

### Zero-Copy Transmission

Leverages io_uring's registered buffers and send-zc paths to minimize
kernel-space copying.

### Buffer Rings (bgid=1)

Enables reclaimed buffers with almost zero synchronization overhead.

### Sparse Fixed File Table

Optimizes descriptor indexing for large connection sets.

### Hugepage-Aligned Memory Layout

Reduces TLB pressure and improves cache locality for high-traffic
workloads.

------------------------------------------------------------------------

## Technical Features

-   **32K+ active connections per process** (scales with CPU count)\
-   **Shared-nothing concurrency**\
-   **Event-driven, lock-free design**\
-   **Fully asynchronous networking pipeline**\
-   **Minimal memory fragmentation**\
-   **SAR-based zero-copy send path**\
-   **Optimized for 10Gbps+ echo throughput**

------------------------------------------------------------------------

## Requirements

-   **Linux Kernel:** 6.1 or newer\
-   **liburing:** 2.2 or newer\
-   **Compiler:** g++ 13+ with C++20 support

------------------------------------------------------------------------

## Build

``` bash
g++ -O3 -march=native io_uring_server.cpp -luring -lpthread -o server
```

------------------------------------------------------------------------

## Run

``` bash
./server
```

------------------------------------------------------------------------

## Notes

-   Designed primarily for benchmarking, research, and high-performance
    system experiments.\
-   Implementation emphasizes clean architecture, predictable behavior,
    and minimal kernel/userspace transitions.

------------------------------------------------------------------------

## Author

This project was engineered by a **15-year-old systems programmer**,
with a focus on mastering modern Linux I/O architecture and
high-performance server design.
