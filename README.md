* Ultra-fast concurrent set (Implementation #1) for integer keys
  using:
  - Minimal spinlock with Intel intrinsics
  - Per-bucket locking
  - Bitwise mask for hashing (requires power-of-two bucket count)
  - Cache-line alignment to reduce false sharing

 * Recommended compiler flags (example):
   g++ -std=c++17 -Ofast -march=native -funroll-loops \
        -finline-functions -fopenmp ...
 
 * Usage:
   #include "concurrent_set_impl1.h"
   Impl1::ConcurrentSet<int> cset;
   cset.Include(42);
   bool exists = cset.Contains(42);
