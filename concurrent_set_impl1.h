/************************************************************
 * concurrent_set_impl1.h
 *
 * Ultra-fast concurrent set for integer keys
 * using:
 *  - Minimal spinlock with Intel intrinsics
 *  - Per-bucket locking
 *  - Bitwise mask for hashing (requires power-of-two bucket count)
 *  - Cache-line alignment to reduce false sharing
 *
 * Recommended compiler flags (example):
 *   g++ -std=c++17 -Ofast -march=native -funroll-loops \
 *       -finline-functions -fopenmp ...
 *
 * Usage:
 *   #include "concurrent_set_impl1.h"
 *   Impl1::ConcurrentSet<int> cset;
 *   cset.Include(42);
 *   bool exists = cset.Contains(42);
 *
 * Author: Manish Arora
 ************************************************************/

 #ifndef CONCURRENT_SET_IMPL1_H
 #define CONCURRENT_SET_IMPL1_H
 
 #include <atomic>
 #include <unordered_set>
 #include <vector>
 #include <thread>
 #include <immintrin.h>    // For _mm_pause()
 #include <cstddef>        // For size_t
 
 namespace Impl1
 {
 
 // Assumes modern CPUs have 64-byte cache lines
 constexpr int kCacheLine = 64;
 
 /**
  * @brief Minimal spinlock that uses _mm_pause()
  *        to reduce contention on busy-wait.
  */
 struct SpinLock {
     std::atomic_flag flag = ATOMIC_FLAG_INIT;
 
     /**
      * @brief Acquire the lock, spinning until it becomes free.
      */
     void lock() {
         while (flag.test_and_set(std::memory_order_acquire)) {
             _mm_pause();  // hint to reduce power and bus contention
         }
     }
 
     /**
      * @brief Release the lock.
      */
     void unlock() {
         flag.clear(std::memory_order_release);
     }
 };
 
 /**
  * @brief A bucket structure that is cache-line aligned.
  *        Each bucket contains its own spinlock and a local unordered_set.
  *
  * @tparam T  The key type (e.g., int).
  */
 template <typename T>
 struct alignas(kCacheLine) Bucket {
     SpinLock lock;
     std::unordered_set<T> set;
 };
 
 /**
  * @brief A concurrent set for integer keys using per-bucket spinlocks
  *        and fast integer hashing via bitwise masking.
  *
  * @tparam T  The key type (integral).
  */
 template <typename T>
 class ConcurrentSet {
 public:
     /**
      * @brief Constructs the concurrent set with a chosen number of buckets.
      *        Default uses a multiple of hardware concurrency.
      *
      * @param buckets  Must be a power of two for bitwise-masking to be correct.
      */
     explicit ConcurrentSet(size_t buckets = optimalBuckets())
         : buckets_(buckets)
     {
         // Optionally ensure that buckets_ is indeed a power of two:
         // (Not strictly required if you trust optimalBuckets() or pass a valid power of two)
     }
 
     /**
      * @brief Thread-safe insertion.
      */
     void Include(const T& item) {
         auto& bucket = buckets_[bucketIndex(item)];
         bucket.lock.lock();
         bucket.set.insert(item);
         bucket.lock.unlock();
     }
 
     /**
      * @brief Thread-safe removal.
      */
     void Exclude(const T& item) {
         auto& bucket = buckets_[bucketIndex(item)];
         bucket.lock.lock();
         bucket.set.erase(item);
         bucket.lock.unlock();
     }
 
     /**
      * @brief Thread-safe membership check.
      *
      * @return true if @p item is present in the set, else false.
      */
     bool Contains(const T& item) {
         auto& bucket = buckets_[bucketIndex(item)];
         bucket.lock.lock();
         bool exists = (bucket.set.find(item) != bucket.set.end());
         bucket.lock.unlock();
         return exists;
     }
 
 private:
     std::vector<Bucket<T>> buckets_;
 
     /**
      * @brief Choose a default number of buckets: 
      *        hardware_concurrency() * 16, fallback 128 if that is zero.
      *        The returned value is forced to be a power of two.
      */
     static size_t optimalBuckets() {
         size_t hc = std::thread::hardware_concurrency();
         size_t desired = hc ? hc * 16 : 128;
 
         // Make sure it's a power of two. This is optional if user ensures it.
         size_t p2 = 1;
         while (p2 < desired) {
             p2 <<= 1;
         }
         return p2;
     }
 
     /**
      * @brief Compute the bucket index using bitwise masking,
      *        which is very fast for integer keys.
      */
     size_t bucketIndex(const T& item) const {
         // item & (buckets_.size() - 1) works if buckets_.size() is a power of two
         return static_cast<size_t>(item) & (buckets_.size() - 1);
     }
 };
 
 } // namespace Impl1
 
 #endif // CONCURRENT_SET_IMPL1_H
