/**
 * @file vector_memory.cpp
 * @brief Vector Memory — AVX2 SIMD Flat Search + Binary Persistence
 *
 * Performance characteristics:
 *   - cosineSimilarityAVX2: 768-dim dot product in 96 iterations (8 floats/cycle)
 *   - Full search of 10,000 vectors: ~2ms on modern CPU
 *   - Memory: 10K × 768 × 4 bytes = ~29MB (single aligned block)
 *   - Persistence: raw binary load = memcpy speed (~10ms for 29MB)
 */

#include "vector_memory.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>
#include <numeric>

// AVX2 intrinsics
#ifdef _MSC_VER
#include <intrin.h>
#include <immintrin.h>
#else
#include <x86intrin.h>
#endif

// JSON for metadata persistence
#include <nlohmann/json.hpp>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define LOG_WARN(...)  spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...)  (void)0
#define LOG_WARN(...)  (void)0
#define LOG_ERROR(...) (void)0
#endif

using json = nlohmann::json;

namespace vision {

// ═══════════════════ Constructor / Destructor ════════════════════════

VectorMemory::VectorMemory() {
    // Allocation deferred until dimension is known (store or load)
}

VectorMemory::~VectorMemory() = default;  // AlignedPtr auto-frees!

void VectorMemory::allocateMatrix(int dim) {
    if (matrix_) return; // already allocated
    dim_ = dim;
    padded_dim_ = ((dim_ + 7) / 8) * 8;
    
    // 32-byte aligned allocation for AVX2 _mm256_load_ps
    // RAII managed via AlignedPtr — auto-freed on destruction/exception
    size_t total_bytes = static_cast<size_t>(kMaxMemoryEntries) * padded_dim_ * sizeof(float);
#ifdef _MSC_VER
    float* raw = static_cast<float*>(_aligned_malloc(total_bytes, 32));
#else
    float* raw = static_cast<float*>(aligned_alloc(32, total_bytes));
#endif
    matrix_.reset(raw);  // AlignedPtr takes ownership
    if (matrix_) {
        std::memset(matrix_.get(), 0, total_bytes);
        LOG_INFO("VectorMemory: Allocated {}MB aligned matrix ({} x {})",
                 total_bytes / (1024 * 1024), kMaxMemoryEntries, padded_dim_);
    } else {
        LOG_ERROR("VectorMemory: Failed to allocate aligned matrix!");
    }
}

// ═══════════════════ SIMD Cosine Similarity ══════════════════════════

float VectorMemory::cosineSimilarityAVX2(const float* a, const float* b, int dim) {
#ifdef __AVX2__
    __m256 sum_ab = _mm256_setzero_ps();  // a·b accumulator
    __m256 sum_aa = _mm256_setzero_ps();  // |a|² accumulator
    __m256 sum_bb = _mm256_setzero_ps();  // |b|² accumulator

    for (int i = 0; i < dim; i += 8) {
        // ALIGNED load — requires 32-byte alignment (we guarantee this)
        __m256 va = _mm256_load_ps(a + i);
        __m256 vb = _mm256_load_ps(b + i);

        // Fused multiply-add: sum += va * vb (single instruction!)
        sum_ab = _mm256_fmadd_ps(va, vb, sum_ab);
        sum_aa = _mm256_fmadd_ps(va, va, sum_aa);
        sum_bb = _mm256_fmadd_ps(vb, vb, sum_bb);
    }

    // Horizontal sum: reduce 8 floats → 1
    // [a0 a1 a2 a3 a4 a5 a6 a7] → a0+a1+a2+a3+a4+a5+a6+a7
    auto hsum = [](__m256 v) -> float {
        __m128 hi = _mm256_extractf128_ps(v, 1);     // [a4 a5 a6 a7]
        __m128 lo = _mm256_castps256_ps128(v);        // [a0 a1 a2 a3]
        __m128 sum4 = _mm_add_ps(lo, hi);             // [a0+a4 a1+a5 a2+a6 a3+a7]
        __m128 shuf = _mm_movehdup_ps(sum4);          // [a1+a5 a1+a5 a3+a7 a3+a7]
        __m128 sum2 = _mm_add_ss(sum4, shuf);         // [a0+a4+a1+a5 ...]
        shuf = _mm_movehl_ps(shuf, sum2);             // [a2+a6+a3+a7 ...]
        __m128 sum1 = _mm_add_ss(sum2, shuf);         // [total ...]
        return _mm_cvtss_f32(sum1);
    };

    float dot = hsum(sum_ab);
    float mag_a = hsum(sum_aa);
    float mag_b = hsum(sum_bb);

    float denom = std::sqrt(mag_a * mag_b);
    return (denom > 1e-8f) ? (dot / denom) : 0.0f;
#else
    return cosineSimilarityScalar(a, b, dim);
#endif
}

float VectorMemory::cosineSimilarityScalar(const float* a, const float* b, int dim) {
    float dot = 0, mag_a = 0, mag_b = 0;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        mag_a += a[i] * a[i];
        mag_b += b[i] * b[i];
    }
    float denom = std::sqrt(mag_a * mag_b);
    return (denom > 1e-8f) ? (dot / denom) : 0.0f;
}

float VectorMemory::cosineSimilarity(const float* a, const float* b, int dim) {
    // BUG 6 FIX: Cache AVX2 check in static local (called once per process, not per-search)
    static const bool hasAVX2 = []() {
#ifdef _MSC_VER
        int cpuInfo[4];
        __cpuid(cpuInfo, 7);
        return (cpuInfo[1] & (1 << 5)) != 0;
#else
        return __builtin_cpu_supports("avx2");
#endif
    }();

    if (hasAVX2) {
        return cosineSimilarityAVX2(a, b, dim);
    }
    return cosineSimilarityScalar(a, b, dim);
}

// ═══════════════════ Store ═══════════════════════════════════════════

bool VectorMemory::store(const std::string& text, const std::string& context,
                          const std::vector<std::string>& tags) {
    if (!embed_fn_) {
        LOG_ERROR("VectorMemory: No embedding function set");
        return false;
    }

    auto embedding = embed_fn_(text);
    if (embedding.empty()) {
        LOG_ERROR("VectorMemory: Returned embedding is empty");
        return false;
    }
    
    // Dynamically set dim if not initialized
    if (dim_ == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dim_ == 0) allocateMatrix(embedding.size());
    }

    if (embedding.size() != dim_) {
        LOG_ERROR("VectorMemory: Embedding length mismatch ({} != {})",
                  embedding.size(), dim_);
        return false;
    }

    return storeWithEmbedding(text, embedding.data(), context, tags);
}

bool VectorMemory::storeWithEmbedding(const std::string& text, const float* embedding,
                                        const std::string& context,
                                        const std::vector<std::string>& tags) {
    if (!matrix_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // Evict oldest if at capacity
    if (count_ >= kMaxMemoryEntries) {
        evictOldest();
    }

    // Copy embedding to aligned matrix row
    // Zero-pad to padded_dim_
    float* dest = row(count_);
    std::memset(dest, 0, padded_dim_ * sizeof(float));
    std::memcpy(dest, embedding, dim_ * sizeof(float));

    // Store metadata
    MemoryEntry entry;
    entry.text = text;
    entry.context = context;
    entry.tags = tags;
    entry.created = std::chrono::steady_clock::now();

    if (count_ < (int)entries_.size()) {
        entries_[count_] = std::move(entry);
    } else {
        entries_.push_back(std::move(entry));
    }

    count_++;
    return true;
}

// ═══════════════════ Search ═════════════════════════════════════════

std::vector<MemorySearchResult> VectorMemory::search(const std::string& query,
                                                       int top_k,
                                                       float min_similarity) const {
    // BUG 8 FIX: Copy embed_fn_ under lock to prevent data race with setEmbeddingFn()
    EmbeddingFn fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn = embed_fn_;
    }
    if (!fn) return {};
    auto embedding = fn(query);
    if (embedding.size() != dim_) return {};
    return searchByEmbedding(embedding.data(), top_k, min_similarity);
}

std::vector<MemorySearchResult> VectorMemory::searchByEmbedding(
    const float* query_embedding, int top_k, float min_similarity) const {

    if (!matrix_ || count_ == 0) return {};

    std::lock_guard<std::mutex> lock(mutex_);

    // RAII aligned query vector — auto-freed on ANY return path
    AlignedPtr aligned_query;
    {
#ifdef _MSC_VER
        float* raw = static_cast<float*>(_aligned_malloc(padded_dim_ * sizeof(float), 32));
#else
        float* raw = static_cast<float*>(aligned_alloc(32, padded_dim_ * sizeof(float)));
#endif
        aligned_query.reset(raw);
    }
    if (!aligned_query) return {};

    std::memset(aligned_query.get(), 0, padded_dim_ * sizeof(float));
    std::memcpy(aligned_query.get(), query_embedding, dim_ * sizeof(float));

    // ── Flat search: compute cosine similarity against ALL vectors ──
    // With AVX2: 10,000 × 768-dim = ~2ms on modern CPU
    struct ScoredIndex {
        int idx;
        float score;
        bool operator>(const ScoredIndex& o) const { return score > o.score; }
    };

    std::vector<ScoredIndex> scores;
    scores.reserve(count_);

    for (int i = 0; i < count_; i++) {
        float sim = cosineSimilarity(aligned_query.get(), row(i), padded_dim_);
        if (sim >= min_similarity) {
            scores.push_back({i, sim});
        }
    }

    // Partial sort: only get top_k (O(n) instead of O(n log n))
    if ((int)scores.size() > top_k) {
        std::partial_sort(scores.begin(), scores.begin() + top_k, scores.end(),
                          std::greater<ScoredIndex>());
        scores.resize(top_k);
    } else {
        std::sort(scores.begin(), scores.end(), std::greater<ScoredIndex>());
    }

    // Build results
    std::vector<MemorySearchResult> results;
    results.reserve(scores.size());
    for (const auto& s : scores) {
        results.push_back({s.idx, s.score, entries_[s.idx]});
    }

    // aligned_query auto-freed by AlignedPtr destructor here!
    return results;
}

std::string VectorMemory::getRelevantContext(const std::string& query, int max_results) const {
    auto results = search(query, max_results);
    if (results.empty()) return "";

    std::string context = "[Relevant memories]:\n";
    for (const auto& r : results) {
        auto age = std::chrono::steady_clock::now() - r.entry.created;
        auto mins = std::chrono::duration_cast<std::chrono::minutes>(age).count();

        context += "- (" + std::to_string((int)(r.similarity * 100)) + "% match, "
                 + std::to_string(mins) + "m ago) "
                 + r.entry.text;
        if (!r.entry.context.empty()) {
            context += " [ctx: " + r.entry.context + "]";
        }
        context += "\n";
    }
    return context;
}

// ═══════════════════ Info ════════════════════════════════════════════

int VectorMemory::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

void VectorMemory::evictOldest() {
    // Find oldest entry
    if (count_ == 0) return;
    int oldest_idx = 0;
    auto oldest_time = entries_[0].created;
    for (int i = 1; i < count_; i++) {
        if (entries_[i].created < oldest_time) {
            oldest_time = entries_[i].created;
            oldest_idx = i;
        }
    }

    // Swap with last entry (O(1) removal from array)
    int last = count_ - 1;
    if (oldest_idx != last) {
        std::swap(entries_[oldest_idx], entries_[last]);
        std::memcpy(row(oldest_idx), row(last), padded_dim_ * sizeof(float));
    }
    count_--;
}

int VectorMemory::purgeOlderThan(int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(seconds);
    int purged = 0;

    // Iterate backwards to avoid shifting issues
    for (int i = count_ - 1; i >= 0; i--) {
        if (entries_[i].created < cutoff) {
            int last = count_ - 1;
            if (i != last) {
                std::swap(entries_[i], entries_[last]);
                std::memcpy(row(i), row(last), padded_dim_ * sizeof(float));
            }
            count_--;
            purged++;
            
            // BUG 13 FIX: After swapping, the new item at index `i` might also be old.
            // Since we're iterating backwards, we must check this index again next loop.
            if (i != last) i++; 
        }
    }
    return purged;
}

// ═══════════════════ Binary Persistence ══════════════════════════════

bool VectorMemory::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("VectorMemory: Cannot open {} for writing", path);
        return false;
    }

    // Write header
    FileHeader header;
    header.count = count_;
    header.dim = dim_;

    // Placeholder for metadata offset (will be filled after matrix write)
    auto header_pos = file.tellp();
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write matrix (raw floats — memcpy speed on load)
    size_t matrix_bytes = static_cast<size_t>(count_) * padded_dim_ * sizeof(float);
    file.write(reinterpret_cast<const char*>(matrix_.get()), matrix_bytes);

    // Record metadata offset
    int64_t metadata_offset = file.tellp();

    // Write metadata as JSON
    json meta = json::array();
    for (int i = 0; i < count_; i++) {
        const auto& e = entries_[i];
        auto age_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - e.created).count();

        meta.push_back({
            {"text", e.text},
            {"context", e.context},
            {"tags", e.tags},
            {"age_seconds", age_s}
        });
    }
    std::string meta_str = meta.dump();
    uint32_t meta_len = static_cast<uint32_t>(meta_str.size());
    file.write(reinterpret_cast<const char*>(&meta_len), sizeof(meta_len));
    file.write(meta_str.data(), meta_str.size());

    // Seek back and write metadata offset in header
    file.seekp(header_pos);
    header.metadata_offset = metadata_offset;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    LOG_INFO("VectorMemory: Saved {} entries to {}", count_, path);
    return true;
}

bool VectorMemory::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    // Read header
    FileHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    // Validate magic
    if (std::memcmp(header.magic, "VMEM0001", 8) != 0) {
        LOG_ERROR("VectorMemory: Invalid file magic in {}", path);
        return false;
    }

    if (header.count > kMaxMemoryEntries || header.dim <= 0) {
        LOG_ERROR("VectorMemory: Incompatible format (count={}, dim={})",
                  header.count, header.dim);
        return false;
    }
    
    allocateMatrix(header.dim);

    // Read matrix (raw memcpy — fastest possible)
    size_t matrix_bytes = static_cast<size_t>(header.count) * padded_dim_ * sizeof(float);
    file.read(reinterpret_cast<char*>(matrix_.get()), matrix_bytes);

    // Read metadata
    file.seekg(header.metadata_offset);
    uint32_t meta_len = 0;
    file.read(reinterpret_cast<char*>(&meta_len), sizeof(meta_len));

    std::string meta_str(meta_len, '\0');
    file.read(meta_str.data(), meta_len);

    auto meta = json::parse(meta_str, nullptr, false);
    if (meta.is_discarded()) {
        LOG_ERROR("VectorMemory: Failed to parse metadata JSON");
        return false;
    }

    // Rebuild entries
    entries_.clear();
    entries_.resize(header.count);
    auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < header.count; i++) {
        const auto& m = meta[i];
        entries_[i].text = m.value("text", "");
        entries_[i].context = m.value("context", "");
        entries_[i].tags = m.value("tags", std::vector<std::string>{});
        int age_s = m.value("age_seconds", 0);
        entries_[i].created = now - std::chrono::seconds(age_s);
    }

    count_ = header.count;
    LOG_INFO("VectorMemory: Loaded {} entries from {}", count_, path);
    return true;
}

} // namespace vision
