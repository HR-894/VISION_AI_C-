#pragma once
/**
 * @file vector_memory.h
 * @brief Vector Memory — Flat Search + AVX2 SIMD Cosine Similarity
 *
 * Architecture:
 *   - 32-byte aligned contiguous float matrix (_aligned_malloc)
 *   - AVX2 _mm256_load_ps for maximum throughput (8 floats/cycle)
 *   - Flat brute-force search (beats HNSW for <10K vectors)
 *   - Binary persistence: [header][matrix][metadata JSON]
 *   - Auto-eviction when capacity exceeded
 *
 * Embedding: External callback (LLMController provides embeddings)
 */

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <chrono>
#include <optional>
#include <cstdint>
#include <memory>

namespace vision {

// ═══════════════════ Data Types ══════════════════════════════════════

static constexpr int kEmbeddingDim = 768;   // nomic-embed-text / MiniLM
static constexpr int kMaxMemoryEntries = 10000;

struct MemoryEntry {
    std::string text;                                // Original text
    std::string context;                             // What was happening (command/response)
    std::vector<std::string> tags;                   // Semantic tags
    std::chrono::steady_clock::time_point created;   // When stored
    float relevance_score = 0.0f;                    // Last search score
};

struct MemorySearchResult {
    int index;
    float similarity;
    const MemoryEntry* entry;  // Non-owning pointer (valid while locked)
};

// ═══════════════════ Vector Memory ═══════════════════════════════════

class VectorMemory {
public:
    /// Embedding callback: text → float[kEmbeddingDim]
    using EmbeddingFn = std::function<std::vector<float>(const std::string&)>;

    VectorMemory();
    ~VectorMemory();

    // Non-copyable (owns aligned memory)
    VectorMemory(const VectorMemory&) = delete;
    VectorMemory& operator=(const VectorMemory&) = delete;

    /// Set embedding function (from LLMController)
    void setEmbeddingFn(EmbeddingFn fn) { embed_fn_ = std::move(fn); }

    // ── Store ──────────────────────────────────────────────────────

    /// Store a text with its context and tags (embedding computed internally)
    bool store(const std::string& text, const std::string& context = "",
               const std::vector<std::string>& tags = {});

    /// Store with pre-computed embedding
    bool storeWithEmbedding(const std::string& text, const float* embedding,
                             const std::string& context = "",
                             const std::vector<std::string>& tags = {});

    // ── Search ─────────────────────────────────────────────────────

    /// Search by text query (embedding computed internally)
    std::vector<MemorySearchResult> search(const std::string& query,
                                            int top_k = 5,
                                            float min_similarity = 0.3f) const;

    /// Search by pre-computed embedding
    std::vector<MemorySearchResult> searchByEmbedding(const float* query_embedding,
                                                       int top_k = 5,
                                                       float min_similarity = 0.3f) const;

    /// Get formatted context string for LLM injection
    std::string getRelevantContext(const std::string& query, int max_results = 3) const;

    // ── Persistence ────────────────────────────────────────────────

    /// Save to binary file
    bool save(const std::string& path) const;

    /// Load from binary file
    bool load(const std::string& path);

    // ── Info ────────────────────────────────────────────────────────

    int size() const;
    int capacity() const { return kMaxMemoryEntries; }
    bool empty() const { return size() == 0; }

    /// Purge entries older than given seconds
    int purgeOlderThan(int seconds);

private:
    // ── Aligned Memory RAII ────────────────────────────────────
    struct AlignedDeleter {
        void operator()(float* p) const {
#ifdef _MSC_VER
            _aligned_free(p);
#else
            free(p);
#endif
        }
    };
    using AlignedPtr = std::unique_ptr<float[], AlignedDeleter>;

    // ── Aligned Matrix ────────────────────────────────────────
    // Single contiguous block: kMaxMemoryEntries × kPaddedDim floats
    // 32-byte aligned for AVX2 _mm256_load_ps
    // RAII managed — auto-freed on destruction/exception/move
    AlignedPtr matrix_;
    int count_ = 0;
    mutable std::mutex mutex_;

    void allocateMatrix();

    /// Get pointer to row i (aligned)
    float* row(int i) { return matrix_.get() + i * kPaddedDim; }
    const float* row(int i) const { return matrix_.get() + i * kPaddedDim; }

    // Pad embedding dim to multiple of 8 for AVX2
    static constexpr int kPaddedDim = ((kEmbeddingDim + 7) / 8) * 8;  // 768 → 768

    // ── Metadata ───────────────────────────────────────────────────
    std::vector<MemoryEntry> entries_;

    // ── Embedding ──────────────────────────────────────────────────
    EmbeddingFn embed_fn_;

    // ── SIMD ───────────────────────────────────────────────────────

    /// AVX2 cosine similarity (32-byte aligned, processes 8 floats/cycle)
    static float cosineSimilarityAVX2(const float* a, const float* b, int dim);

    /// Scalar fallback for non-AVX2 CPUs
    static float cosineSimilarityScalar(const float* a, const float* b, int dim);

    /// Auto-detect: use AVX2 if available, else scalar
    static float cosineSimilarity(const float* a, const float* b, int dim);

    // ── Eviction ───────────────────────────────────────────────────
    void evictOldest();

    // ── Binary format ──────────────────────────────────────────────
    struct FileHeader {
        char magic[8] = {'V','M','E','M','0','0','0','1'};  // "VMEM0001"
        int32_t count = 0;
        int32_t dim = kEmbeddingDim;
        int64_t metadata_offset = 0;  // Byte offset to JSON metadata
    };
};

} // namespace vision
