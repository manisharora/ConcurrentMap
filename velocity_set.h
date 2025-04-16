/************************************************************
 * velocity_set.h
 *
 * VelocitySet: An ultra-fast concurrent set for integer keys.
 * Implementation uses:
 *  - Minimal spinlock with Intel intrinsics (`_mm_pause`)
 *  - Fine-grained per-bucket locking
 *  - Fast bitwise mask hashing (requires power-of-two bucket count)
 *  - Cache-line alignment to reduce false sharing
 *
 * Recommended compiler flags (example):
 *   g++ -std=c++17 -O3 -march=native -funroll-loops \
 *       -finline-functions -pthread ...
 *
 * Usage:
 *   #include "velocity_set.h"
 *   velocity::VelocitySet<int> vset;
 *   vset.Insert(42);
 *   bool exists = vset.Contains(42);
 *   vset.Remove(42);
 *
 * Author: Manish Arora
 ************************************************************/

#ifndef VELOCITY_SET_H
#define VELOCITY_SET_H

#include <atomic>
#include <unordered_set>
#include <vector>
#include <thread>         // For std::thread::hardware_concurrency
#include <immintrin.h>    // For _mm_pause() - x86/x64 specific
#include <cstddef>        // For size_t
#include <type_traits>    // For std::is_integral
#include <stdexcept>      // For std::invalid_argument
#include <cmath>          // For std::log2, std::ceil
#include <limits>         // For std::numeric_limits

// Pre-check for potential non-x86 compilation if intrinsics are essential
#if !defined(__x86_64__) && !defined(_M_X64) && !defined(__i386__) && !defined(_M_IX86)
    #warning "VelocitySet SpinLock uses _mm_pause(), specific to x86/x64. Performance or behavior might differ on other architectures without adaptation."
    // Consider providing a fallback or requiring a specific define for non-x86
#endif

namespace velocity
{

// --- Configuration ---
constexpr int kCacheLineSize = 64; // Assumed cache line size for alignment

/**
 * @brief Minimalist SpinLock using CPU-relax hints.
 *
 * Uses `_mm_pause()` on x86/x64 to yield execution resources during
 * busy-waiting, reducing power consumption and contention on hyper-threads.
 * Falls back to a simple test-and-set loop if intrinsics are unavailable
 * (though performance may degrade significantly under contention).
 */
struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

    /** @brief Acquires the lock, spinning until successful. */
    void lock() noexcept {
        while (flag.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause(); // Intrinsics for x86/x64
#else
            // Basic busy-wait for other architectures or if intrinsics are disabled
            // Consider std::this_thread::yield() as an alternative,
            // but its behavior/performance varies widely.
#endif
        }
    }

    /** @brief Releases the lock. */
    void unlock() noexcept {
        flag.clear(std::memory_order_release);
    }

    // Non-copyable and non-movable
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;
    SpinLock() = default; // Ensure default constructor is available
};


/**
 * @brief A cache-line aligned bucket holding a lock and the actual data set.
 *
 * Aligning ensures that different buckets are less likely to cause
 * false sharing cache contention when accessed by different threads/cores.
 *
 * @tparam T The integer key type stored in the set.
 */
template <typename T>
struct alignas(kCacheLineSize) Bucket {
    SpinLock lock;
    std::unordered_set<T> data_set;

    // Default constructor needed for vector initialization
    Bucket() = default;

    // Explicitly define move constructor (rule of 5 if others are defined/deleted)
    // Note: Moving buckets is generally not intended after construction
    // due to the nature of the hash set, but required by vector sometimes.
    Bucket(Bucket&& other) noexcept
        : lock(), // Lock state is not transferred
          data_set(std::move(other.data_set))
    {}

    // Explicitly define move assignment operator
    Bucket& operator=(Bucket&& other) noexcept {
        if (this != &other) {
            // Lock state is reset, not transferred
            data_set = std::move(other.data_set);
        }
        return *this;
    }

    // Prevent copying
    Bucket(const Bucket&) = delete;
    Bucket& operator=(const Bucket&) = delete;
};


/**
 * @brief VelocitySet: An ultra-fast concurrent set for integer keys.
 *
 * Optimizes for high-throughput concurrent access using fine-grained locking,
 * fast hashing, and cache-aware design.
 *
 * @tparam T Must be an integral type (int, uint32_t, size_t, etc.).
 */
template <typename T>
class VelocitySet {
    // Static assertion to ensure T is an integral type
    static_assert(std::is_integral_v<T>, "VelocitySet requires an integral key type (e.g., int, size_t).");

public:
    /**
     * @brief Constructs the concurrent set with a specified number of buckets.
     *
     * @param bucket_count The desired number of buckets. **Must be a power of two**
     *                     for the fast bitwise hashing to work correctly. If 0 is
     *                     passed, a default power-of-two size is calculated based
     *                     on hardware concurrency.
     * @throws std::invalid_argument if bucket_count is non-zero and not a power of two.
     */
    explicit VelocitySet(size_t bucket_count = 0)
    {
        if (bucket_count == 0) {
            buckets_count_ = calculate_default_buckets();
        } else {
            if (!is_power_of_two(bucket_count)) {
                throw std::invalid_argument("VelocitySet: bucket_count must be a power of two.");
            }
            buckets_count_ = bucket_count;
        }
        // The mask relies on buckets_count_ being a power of two
        bucket_mask_ = buckets_count_ - 1;
        // Initialize the buckets vector
        buckets_.resize(buckets_count_);
    }

    /**
     * @brief Inserts an item into the set (thread-safe).
     * @param item The integer item to insert.
     */
    void Insert(const T& item) noexcept {
        Bucket<T>& bucket = get_bucket(item);
        bucket.lock.lock();
        bucket.data_set.insert(item); // std::unordered_set handles duplicates
        bucket.lock.unlock();
    }

    /**
     * @brief Removes an item from the set (thread-safe).
     * @param item The integer item to remove.
     */
    void Remove(const T& item) noexcept {
        Bucket<T>& bucket = get_bucket(item);
        bucket.lock.lock();
        bucket.data_set.erase(item);
        bucket.lock.unlock();
    }

    /**
     * @brief Checks if an item exists in the set (thread-safe).
     * @param item The integer item to check for.
     * @return true if the item is present, false otherwise.
     */
    bool Contains(const T& item) noexcept {
        Bucket<T>& bucket = get_bucket(item);
        bucket.lock.lock();
        // Use count for potentially faster check than find != end in some impls
        bool exists = (bucket.data_set.count(item) > 0);
        bucket.lock.unlock();
        return exists;
    }

    /**
     * @brief Returns the number of buckets being used.
     * @return The number of buckets (always a power of two).
     */
    size_t GetBucketCount() const noexcept {
        return buckets_count_;
    }

    /**
     * @brief Clears all elements from the set (thread-safe, potentially blocking).
     * Note: This acquires locks on *all* buckets sequentially. Avoid calling
     * concurrently with heavy insert/remove/contains operations if possible.
     */
    void Clear() noexcept {
        for (size_t i = 0; i < buckets_count_; ++i) {
            buckets_[i].lock.lock();
            buckets_[i].data_set.clear();
            buckets_[i].lock.unlock(); // Release lock immediately after clearing bucket
        }
    }

    /**
     * @brief Returns the approximate total number of elements in the set.
     * Note: This is potentially expensive and provides only an estimate if
     * called concurrently with modifications, as it locks buckets sequentially.
     * Use primarily for debugging or diagnostics.
     * @return Approximate number of elements.
     */
    size_t GetApproximateSize() noexcept {
        size_t total_size = 0;
        for (size_t i = 0; i < buckets_count_; ++i) {
            buckets_[i].lock.lock();
            total_size += buckets_[i].data_set.size();
            buckets_[i].lock.unlock();
        }
        return total_size;
    }


private:
    std::vector<Bucket<T>> buckets_;
    size_t buckets_count_; // Store the count (power of two)
    size_t bucket_mask_;   // Cache mask for hashing (bucket_count - 1)

    /**
     * @brief Calculates a default power-of-two number of buckets.
     * Aims for a value significantly larger than hardware concurrency.
     * @return A power-of-two number of buckets.
     */
    static size_t calculate_default_buckets() noexcept {
        // Start with a reasonable minimum
        size_t desired_buckets = 128;
        unsigned int hw_threads = std::thread::hardware_concurrency();

        // If hardware_concurrency is available and reasonable, scale by it
        if (hw_threads > 0 && hw_threads < (std::numeric_limits<unsigned int>::max() / 16)) {
             // Multiply by a factor (e.g., 16) to reduce contention per thread
            desired_buckets = std::max(desired_buckets, static_cast<size_t>(hw_threads * 16));
        }

        // Find the next power of two >= desired_buckets
        return next_power_of_two(desired_buckets);
    }

    /**
     * @brief Finds the smallest power of two greater than or equal to n.
     * @param n The input number.
     * @return The next power of two. Returns 1 if n is 0.
     */
    static size_t next_power_of_two(size_t n) noexcept {
        if (n == 0) return 1;
        // Efficient bit manipulation way:
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        if constexpr (sizeof(size_t) > 4) { // Handle 64-bit size_t
             n |= n >> 32;
        }
        n++;
        return n;

        // Alternative using floating point (potentially slightly less performant/precise for edge cases):
        // if (n == 0) return 1;
        // return static_cast<size_t>(1) << static_cast<size_t>(std::ceil(std::log2(static_cast<double>(n))));
    }

    /**
     * @brief Checks if a number is a power of two.
     * @param n The number to check.
     * @return true if n is > 0 and a power of two, false otherwise.
     */
    static bool is_power_of_two(size_t n) noexcept {
        return (n > 0) && ((n & (n - 1)) == 0);
    }

    /**
     * @brief Computes the target bucket index using fast bitwise masking.
     * Relies on `buckets_count_` being a power of two.
     * @param item The item to hash.
     * @return The index of the bucket for the item.
     */
    size_t hash_to_index(const T& item) const noexcept {
        // Cast to size_t for bitwise operation. Handles signed/unsigned T.
        // The '& bucket_mask_' performs a modulo operation when bucket_mask_ is (power_of_two - 1).
        return static_cast<size_t>(item) & bucket_mask_;
    }

    /**
     * @brief Gets a reference to the appropriate bucket for a given item.
     * @param item The item whose bucket is needed.
     * @return A non-const reference to the Bucket.
     */
    Bucket<T>& get_bucket(const T& item) noexcept {
        return buckets_[hash_to_index(item)];
    }

    /**
     * @brief Gets a const reference to the appropriate bucket for a given item.
     * (Currently unused publicly, but good practice to have)
     * @param item The item whose bucket is needed.
     * @return A const reference to the Bucket.
     */
    // const Bucket<T>& get_bucket(const T& item) const noexcept {
    //     return buckets_[hash_to_index(item)];
    // }
};

} // namespace velocity

#endif // VELOCITY_SET_H
