# 🚀 VelocitySet: A Blazing-Fast Concurrent Integer Set for C++ 🚀

[![Language](https://img.shields.io/badge/C%2B%2B-17%2B-blue.svg)](https://isocpp.org/std/the-standard)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE) <!-- Make sure you have an MIT LICENSE file -->
[![Type](https://img.shields.io/badge/Type-Header--Only-orange.svg)]()
[![Platform](https://img.shields.io/badge/Platform-x86--64%20Optimized-informational.svg)]()
[![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen.svg)]() <!-- Optional: Replace with real CI badge -->

`VelocitySet` is a **header-only C++17 library** providing an **ultra-fast concurrent hash set** specifically tailored for **integral types** (`int`, `long`, `size_t`, etc.). It's meticulously optimized for scenarios demanding high-throughput, low-latency thread-safe set operations on integers, leveraging modern CPU features and careful design.

Think lock contention is slowing down your multi-threaded integer lookups or insertions? Give `VelocitySet` a try! 🏎️💨

---

## ✨ Core Features ✨

*   **⚡ Lightning Speed:** Designed ground-up for maximum performance in multi-threaded environments dealing with integer keys.
*   **🔒 Fine-Grained Locking:** Employs **per-bucket spinlocks**. This drastically reduces lock contention compared to a single global lock, allowing multiple threads to operate on different parts of the set concurrently.
*   **🧠 CPU-Aware Spinlock:** Utilizes a minimal `SpinLock` featuring the `_mm_pause()` intrinsic (on x86/x64). This hint helps the CPU manage resources better during spin-waiting, reducing power consumption and improving performance on hyper-threaded cores compared to naive busy-loops.
*   **💡 Blazing-Fast Hashing:** Uses **bitwise AND masking** (`key & (bucket_count - 1)`) for hashing. This is significantly faster than typical modulo operations for integers.
    *   ⚠️ **Requirement:** This technique mandates that the **number of buckets must be a power of two**. `VelocitySet` enforces this or calculates a suitable default.
*   **📦 Cache-Friendly Design:** Each internal `Bucket` (lock + `std::unordered_set`) is aligned to **64-byte cache lines** (`alignas(64)`). This minimizes "false sharing" – a performance killer where threads accessing different data unintentionally invalidate each other's CPU caches because the data happens to reside on the same cache line.
*   **🧩 Header-Only Integration:** Simply `#include "velocity_set.h"`. No separate compilation or linking needed for the library itself. Easy to drop into any C++17 project.
*   **✔️ Type Safety:** Uses `static_assert` to ensure template parameter `T` is an integral type at compile time.

---

## 🤔 Why Choose VelocitySet?

Use `VelocitySet` when:

*   ✅ You need a thread-safe set for **integer types** (`int`, `uint32_t`, `size_t`, etc.).
*   ✅ **Performance is critical**, especially lookup (`Contains`) and insertion (`Insert`) speed under concurrency.
*   ✅ Your application runs on **multiple CPU cores** and can benefit from fine-grained locking.
*   ✅ You are targeting **x86-64 architectures** where `_mm_pause()` provides benefits (though it may function on others).

Consider alternatives if:

*   ❌ You need to store non-integral types (strings, custom objects).
*   ❌ You expect **extremely high, sustained contention** on *exactly* the same few buckets, where a blocking mutex *might* eventually be more CPU-friendly (though likely slower).
*   ❌ Your critical sections *within* the lock (if you were to hypothetically modify the internals) are very long. Spinlocks shine with short critical sections.
*   ❌ Strict portability to non-x86 architectures is paramount *without modification*.

---

## ⚙️ Requirements

*   **C++17 Compliant Compiler:** (e.g., GCC 7+, Clang 5+, MSVC 19.14+).
*   **Standard Library Headers:** `<atomic>`, `<vector>`, `<unordered_set>`, `<thread>`, `<cstddef>`, `<type_traits>`, `<stdexcept>`, `<cmath>`, `<limits>`.
*   **x86/x64 Architecture:** Recommended for full performance due to `_mm_pause()`. The code includes a fallback, but performance under contention may differ on other architectures. Requires `<immintrin.h>` (typically available with modern GCC/Clang/MSVC on x86-64).

---

## 🛠️ Installation

1.  Copy `velocity_set.h` into your project's include path.
2.  Include the header in your C++ files:

    ```cpp
    #include "velocity_set.h"
    ```

---

## 🚀 Quick Start & Usage Example

```cpp
#include "velocity_set.h"
#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <numeric> // For std::iota
#include <atomic>  // For std::atomic

int main() {
    // === Initialization ===
    // Use default bucket count (recommended: calculates power-of-two based on hardware)
    velocity::VelocitySet<int> vset;
    std::cout << "🏁 Initialized VelocitySet with " << vset.GetBucketCount() << " buckets." << std::endl;

    // Or, specify a custom power-of-two bucket count
    // velocity::VelocitySet<int> vset_custom(2048); // Must be power of two!

    // === Concurrent Operations ===
    int num_threads = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 4;
    int items_total = 100'000;
    int items_per_thread = items_total / num_threads;

    std::vector<std::thread> threads;
    std::atomic<int> insert_count = 0;
    std::atomic<int> remove_count = 0;
    std::atomic<int> found_count = 0;

    std::cout << "🚀 Launching " << num_threads << " threads to process " << items_total << " items..." << std::endl;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            int start_item = t * items_per_thread;
            int end_item = (t + 1) * items_per_thread;

            // Phase 1: Insertion
            for (int i = start_item; i < end_item; ++i) {
                vset.Insert(i);
                insert_count++;
            }

            // Phase 2: Check and Remove Evens
            for (int i = start_item; i < end_item; ++i) {
                if (vset.Contains(i)) {
                    found_count++;
                    if (i % 2 == 0) { // Remove even numbers
                        vset.Remove(i);
                        remove_count++;
                    }
                }
            }
        });
    }

    // Wait for all threads to complete
    for (auto& th : threads) {
        th.join();
    }
    std::cout << "✅ All threads finished." << std::endl;

    // === Verification ===
    std::cout << "\n📊 Verification Results:" << std::endl;
    std::cout << "  Items Inserted: " << insert_count.load() << std::endl;
    std::cout << "  Items Found Initially: " << found_count.load() << std::endl;
    std::cout << "  Items Removed (Evens): " << remove_count.load() << std::endl;

    // Final checks (adjust range if items_total is small)
    assert(vset.Contains(1));    // Odd number should still be present
    assert(!vset.Contains(0));   // Even number should have been removed
    assert(vset.Contains(99999)); // Last odd number (adjust if items_total changes)
    assert(!vset.Contains(99998)); // Last even number (adjust if items_total changes)

    size_t approx_final_size = vset.GetApproximateSize();
    std::cout << "  Approximate Final Size: " << approx_final_size << std::endl;
    // Expected size is roughly items_total / 2 (only odds remaining)
    assert(approx_final_size > (items_total / 3) && approx_final_size < (items_total * 2 / 3));

    std::cout << "\n🎉 VelocitySet operations completed successfully!" << std::endl;

    return 0;
}
