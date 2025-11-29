Hyper Uring Server

A ultra-high performance TCP server built on io_uring, designed with:

Worker-per-core architecture

Zero-copy send

Multishot accept + multishot recv

Buffer rings (bgid=1)

Sparse fixed file table

Shared-nothing concurrency model

Hugepages optimized memory layout

32K connections per process + multi-process scaling

Requirements

Linux kernel 6.1+

liburing 2.2+

g++ 13+ (C++20)

Build
g++ -O3 -march=native io_uring_server.cpp -luring -lpthread -o server

Run
./server

Notes

Each CPU core spawns a dedicated process.

Uses zero-copy send (SAR) for minimal latency.

Designed for high-throughput echo benchmarks (10Gbps+ easily).

Author

This project was developed by a 15-year-old developer exploring advanced low-level Linux networking, io_uring internals, and high-performance server design.
