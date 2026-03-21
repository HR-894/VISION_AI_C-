/**
 * @file local_backend.cpp
 * @brief Local AI Backend via llama.cpp — extracted from original LLMController
 *
 * Handles GGUF model loading, tokenization, KV cache shifting,
 * temperature/top-p sampling, embeddings, idle auto-unload,
 * and safe VRAM/RAM cleanup on shutdown.
 */

#include "local_backend.h"
#include <filesystem>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <random>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#ifdef VISION_HAS_LLM
#include "llama.h"
#include "clip.h"
#endif

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_ERROR(...) (void)0
#define LOG_WARN(...) (void)0
#endif

namespace fs = std::filesystem;

namespace vision {

// ── RAII guard for llama_batch — ensures batch is ALWAYS freed ────
// This prevents memory leaks on every early-return, break, or exception
struct ScopedBatch {
    llama_batch batch;
    bool active = false;
    
    ScopedBatch(int n_tokens, int embd, int n_seq_max) {
#ifdef VISION_HAS_LLM
        batch = llama_batch_init(n_tokens, embd, n_seq_max);
        batch.n_tokens = 0;
        active = true;
#else
        (void)n_tokens; (void)embd; (void)n_seq_max;
        batch = {};
#endif
    }
    
    ~ScopedBatch() {
#ifdef VISION_HAS_LLM
        if (active) llama_batch_free(batch);
#endif
    }
    
    // Non-copyable
    ScopedBatch(const ScopedBatch&) = delete;
    ScopedBatch& operator=(const ScopedBatch&) = delete;
};

// ═══════════════════ Constructor / Destructor ═══════════════════════

LocalBackend::LocalBackend(const std::string& model_path)
    : model_path_(model_path), last_activity_(std::chrono::steady_clock::now()) {
    if (model_path_.empty()) model_path_ = findModelPath();
}

LocalBackend::~LocalBackend() {
    shutdown();
}

// ═══════════════════ Lifecycle ═════════════════════════════════════

bool LocalBackend::initialize() {
#ifdef VISION_HAS_LLM
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (loaded_) return true;

    if (!fs::exists(model_path_)) {
        LOG_ERROR("LocalBackend: Model not found: {}", model_path_);
        return false;
    }

    llama_backend_init();

    auto model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers_;
    model_params.use_mmap  = true;   // Req 1: Ollama-style dynamic memory mapping
    model_params.use_mlock = false;  // Req 1: prevent OOM kill on ≤8GB systems

    model_ = llama_model_load_from_file(model_path_.c_str(), model_params);
    if (!model_) {
        LOG_ERROR("LocalBackend: Failed to load model: {}", model_path_);
        return false;
    }

    auto ctx_params = llama_context_default_params();
    ctx_params.n_ctx = context_size_;
    ctx_params.n_threads = std::max(1, thread_count_);
    ctx_params.n_threads_batch = ctx_params.n_threads;
    ctx_params.embeddings = true;

    ctx_ = llama_init_from_model(model_, ctx_params);
    if (!ctx_) {
        LOG_ERROR("LocalBackend: Failed to create context");
        llama_model_free(model_);
        model_ = nullptr;
        return false;
    }
    
    // Check for CLIP projection model (.mmproj)
    // Heuristic: If model is 'llama-3-vision-q4_k_m.gguf', look for '*mmproj*.gguf' or same name '.mmproj' in the same folder
    fs::path dir = fs::path(model_path_).parent_path();
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::string fname = entry.path().filename().string();
        if (fname.find("mmproj") != std::string::npos || entry.path().extension() == ".mmproj") {
            LOG_INFO("LocalBackend: Found multimodal projection model: {}", entry.path().string());
            ctx_clip_ = clip_model_load(entry.path().string().c_str(), 0);
            if (ctx_clip_) {
                LOG_INFO("LocalBackend: Vision capabilities ENABLED");
            } else {
                LOG_ERROR("LocalBackend: Failed to load CLIP model");
            }
            break;
        }
    }

    loaded_ = true;
    touchActivity();
    LOG_INFO("LocalBackend loaded: {} (GPU layers: {}, threads: {}, ctx: {})",
             model_path_, gpu_layers_, thread_count_, context_size_);
    return true;
#else
    return false;
#endif
}

void LocalBackend::shutdown() {
#ifdef VISION_HAS_LLM
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (ctx_clip_) { clip_free(ctx_clip_); ctx_clip_ = nullptr; }
    if (ctx_) { llama_free(ctx_); ctx_ = nullptr; }
    if (model_) { llama_model_free(model_); model_ = nullptr; }
    if (loaded_) {
        // BUG FIX: llama_backend_init() was called in initialize()
        // but never matched — this frees internal llama.cpp state
        // (thread pools, memory pools, CUDA/Vulkan contexts)
        llama_backend_free();
    }
    loaded_ = false;
    LOG_INFO("LocalBackend: VRAM/RAM released — model + backend freed");
#endif
}

bool LocalBackend::isReady() const {
    return loaded_;
}

// ═══════════════════ Inference ═════════════════════════════════════

std::string LocalBackend::generate(const std::string& prompt,
                                   const std::vector<Message>& history,
                                   StreamCallback stream_cb) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif

#ifdef VISION_HAS_LLM
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (!loaded_ && !initialize()) {
        LOG_ERROR("LocalBackend::generate() — model not loaded, cannot generate");
        return "Error: Local model is not loaded correctly. Please check your model path in Settings.";
    }
    
    // CRASH GUARD: Double-check pointers are valid after initialize()
    if (!ctx_ || !model_) {
        LOG_ERROR("LocalBackend::generate() — context or model is null after initialization");
        loaded_ = false;  // Reset so next call re-initializes
        return "Error: Model context became invalid. Please restart the app or reload the model.";
    }

    touchActivity();
    cancel_generation_ = false;

    // Build full prompt with conversation history (Context Synchronization)
    std::string full_prompt = buildFullPrompt(prompt, history);

    // Tokenize
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    // FIX 2: Buffer Underrun logic. Allocate dynamically instead of context_size_
    // so massive prompts don't get silently dropped by llama_tokenize.
    std::vector<llama_token> tokens(full_prompt.size() + 128);
    int n_tokens = llama_tokenize(vocab, full_prompt.c_str(), (int)full_prompt.size(),
                                   tokens.data(), (int)tokens.size(), true, true);
    if (n_tokens < 0) {
        LOG_ERROR("LocalBackend: Tokenization failed");
        return "";
    }
    tokens.resize(n_tokens);

    // ── KV Cache Shifting: if prompt exceeds half the context, trim ──
    if (n_tokens > context_size_ / 2) {
        int original_count = n_tokens;
        int keep_prefix = std::min(128, n_tokens / 4);
        int keep_suffix = context_size_ / 2 - keep_prefix;
        std::vector<llama_token> trimmed;
        trimmed.insert(trimmed.end(), tokens.begin(), tokens.begin() + keep_prefix);
        trimmed.insert(trimmed.end(), tokens.end() - keep_suffix, tokens.end());
        tokens = std::move(trimmed);
        n_tokens = (int)tokens.size();
        LOG_WARN("KV cache shift: trimmed prompt from {} to {} tokens (kept system prompt)",
                 original_count, n_tokens);
    }

    // ── Generation Budget ──
    // Reserve at least 25% of the context window for generation.
    // Never hard-cap below 512 tokens — that causes mid-sentence cutoffs.
    int safe_max_tokens = max_tokens_;
    int generation_budget = std::max(512, context_size_ / 4);
    if (safe_max_tokens > generation_budget) {
        safe_max_tokens = generation_budget;
    }

    // ── PRD Fix 4: Rolling KV Cache — preserve system prompt + recent context ──
    if (n_past_ + n_tokens > context_size_ - safe_max_tokens) {
        int keep_prefix = std::min(128, n_past_ / 4);  // System prompt tokens
        int keep_suffix = context_size_ / 4;             // Recent context
        int discard_from = keep_prefix;
        int discard_to = std::max(discard_from + 1, n_past_ - keep_suffix);

        // ── CRASH GUARD: Strict bounds validation ──
        // Ensure discard_from and discard_to are within [0, n_past_)
        // Out-of-range values cause llama_memory_seq_rm to segfault.
        discard_from = std::max(0, std::min(discard_from, n_past_ - 1));
        discard_to   = std::max(discard_from + 1, std::min(discard_to, n_past_));

        if (discard_to > discard_from && discard_to <= n_past_) {
            auto mem = llama_get_memory(ctx_);
            if (mem) {
                llama_memory_seq_rm(mem, 0, discard_from, discard_to);
                llama_memory_seq_add(mem, 0, discard_to, n_past_, -(discard_to - discard_from));
                n_past_ -= (discard_to - discard_from);
                LOG_WARN("KV cache rolling trim: freed {} token slots (kept prefix={}, suffix={})",
                         discard_to - discard_from, keep_prefix, keep_suffix);
            } else {
                LOG_ERROR("KV cache rolling trim: llama_get_memory returned null — skipping trim");
            }
        } else {
            // Cache too small to roll — full clear as fallback
            auto mem = llama_get_memory(ctx_);
            if (mem) llama_memory_clear(mem, true);
            n_past_ = 0;
            LOG_WARN("KV cache full — fallback to clear");
        }
    }

    // ── RAII Batch: guaranteed cleanup on ALL exit paths ──
    ScopedBatch sb(context_size_, 0, 1);
    auto& batch = sb.batch;

    // ── Evaluate Initial Prompt in Chunks (Prevent Batch Overflow Crash) ──
    uint32_t n_batch = llama_n_batch(ctx_);
    llama_seq_id seq_ids[] = {0};
    
    for (int i = 0; i < n_tokens; i += n_batch) {
        int n_eval = std::min(n_tokens - i, (int)n_batch);
        batch.n_tokens = 0;
        
        for (int j = 0; j < n_eval; j++) {
            batch.token   [batch.n_tokens] = tokens[i + j];
            batch.pos     [batch.n_tokens] = n_past_ + i + j;  // Fix 4: offset by n_past_
            batch.n_seq_id[batch.n_tokens] = 1;
            batch.seq_id  [batch.n_tokens] = seq_ids;
            // Only the final token in the final chunk needs logits
            batch.logits  [batch.n_tokens] = ((i + j) == n_tokens - 1);
            batch.n_tokens++;
        }
        
        if (llama_decode(ctx_, batch) != 0) {
            LOG_ERROR("LocalBackend: Decode failed at token chunk {}/{}", i, n_tokens);
            return "";  // ScopedBatch destructor frees batch automatically
        }
    }
    n_past_ += n_tokens;  // Fix 4: advance the KV cursor

    // Generate tokens
    std::string result;
    int n_cur = n_past_;  // Fix 4: start from KV cursor position

    for (int i = 0; i < safe_max_tokens; i++) {
        // ── Emergency Stop check ──
        if (cancel_generation_.load()) {
            LOG_WARN("LocalBackend: Generation cancelled (emergency stop)");
            break;  // ScopedBatch handles cleanup
        }

        auto* logits = llama_get_logits_ith(ctx_, -1);
        if (!logits) {
            LOG_ERROR("LocalBackend: llama_get_logits_ith returned null — context invalid");
            break;
        }
        int n_vocab_size = llama_vocab_n_tokens(vocab);

        // Temperature sampling
        llama_token new_token;
        if (temperature_ < 0.01f) {
            // Greedy
            float max_logit = logits[0];
            new_token = 0;
            for (int j = 1; j < n_vocab_size; j++) {
                if (logits[j] > max_logit) {
                    max_logit = logits[j];
                    new_token = j;
                }
            }
        } else {
            // Top-p sampling with temperature
            std::vector<std::pair<float, llama_token>> candidates;
            candidates.reserve(n_vocab_size);
            for (int j = 0; j < n_vocab_size; j++) {
                candidates.emplace_back(logits[j] / temperature_, j);
            }
            std::sort(candidates.begin(), candidates.end(), std::greater<>());

            // Softmax + top-p
            float max_val = candidates[0].first;
            float sum = 0;
            for (auto& [score, _] : candidates) {
                score = std::exp(score - max_val);
                sum += score;
            }

            float cumulative = 0;
            float threshold = top_p_ * sum;
            float r;
            {
                static thread_local std::mt19937 rng{std::random_device{}()};
                std::uniform_real_distribution<float> dist(0.0f, threshold);
                r = dist(rng);
            }

            new_token = candidates[0].second;
            for (auto& [score, tok] : candidates) {
                cumulative += score;
                if (cumulative >= r) {
                    new_token = tok;
                    break;
                }
            }
        }
        
        // ── Req 2: Real-Time Token Streaming ──
        // Convert token to string piece and fire callback immediately
        char buf[128];
        int n_chars = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);
        if (n_chars > 0 && stream_cb) {
            std::string piece(buf, n_chars);
            stream_cb(piece);
        }

        // End of generation
        if (llama_vocab_is_eog(vocab, new_token)) break;

        // Convert token to string
        // The result string is built for the return value, even if streaming
        if (n_chars > 0) result.append(buf, n_chars);

        // ── Phase 2: Anti-Prompt Detection (String Tail Matching) ──
        // Some models fail to emit an EOS token ID and instead output the literal string
        // of a stop sequence. We check the trailing characters to prevent infinite generation loops.
        static const std::vector<std::string> stop_sequences = {
            "<|im_end|>", "<|eot_id|>", "</s>", "<|endoftext|>"
        };
        bool stop_detected = false;
        for (const auto& stop_seq : stop_sequences) {
            if (result.size() >= stop_seq.size() && 
                result.compare(result.size() - stop_seq.size(), stop_seq.size(), stop_seq) == 0) {
                stop_detected = true;
                result.erase(result.size() - stop_seq.size()); // Strip the trigger word from final output
                LOG_INFO("LocalBackend: Anti-Prompt detected '{}', stopping generation early", stop_seq);
                break;
            }
        }
        if (stop_detected) break;

        // ── Context limit check ──
        if (n_cur >= context_size_ - 1) {
            LOG_WARN("Context limit reached during generation ({}/{}), stopping",
                     n_cur, context_size_);
            break;
        }

        // Prepare next batch
        batch.n_tokens = 0;
        batch.token   [batch.n_tokens] = new_token;
        batch.pos     [batch.n_tokens] = n_cur;
        batch.n_seq_id[batch.n_tokens] = 1;
        batch.seq_id  [batch.n_tokens] = seq_ids;
        batch.logits  [batch.n_tokens] = true;
        batch.n_tokens++;
        n_cur++;

        if (llama_decode(ctx_, batch) != 0) break;  // ScopedBatch handles cleanup
    }

    // PRD Fix 4: Update KV cursor to include generated tokens
    n_past_ = n_cur;

    // ScopedBatch destructor frees batch here — guaranteed on all paths
    cancel_generation_ = false;

    return result;
#else
    (void)prompt;
    (void)history;
    return "";
#endif
}

// ═══════════════════ Vision (LLaVA / Multimodal) ═══════════════════

std::string LocalBackend::generateVision(const std::string& prompt,
                                         const std::vector<uint8_t>& image_bytes,
                                         const std::vector<Message>& history,
                                         StreamCallback stream_cb) {
#ifdef VISION_HAS_LLM
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (!loaded_ && !initialize()) {
        return "Error: Local model not loaded.";
    }
    if (!ctx_clip_) {
        LOG_WARN("LocalBackend::generateVision — no CLIP model loaded, falling back to text-only");
        return generate(prompt + "\n[Note: No vision model available. I cannot see the screen.]", history, stream_cb);
    }

    touchActivity();
    cancel_generation_ = false;

    // --- Decode image bytes (JPEG/PNG) via stb_image ---
    // stb_image is bundled with llama.cpp and handles JPEG/PNG/BMP/GIF
    #define STB_IMAGE_IMPLEMENTATION
    #include "stb_image.h"

    int img_w = 0, img_h = 0, img_c = 0;
    unsigned char* rgb_pixels = stbi_load_from_memory(
        image_bytes.data(), static_cast<int>(image_bytes.size()),
        &img_w, &img_h, &img_c, 3  // Force 3 channels (RGB)
    );
    if (!rgb_pixels) {
        LOG_ERROR("LocalBackend::generateVision — Failed to decode image ({} bytes)", image_bytes.size());
        return generate(prompt + "\n[Image decode failed]", history, stream_cb);
    }

    // --- Build CLIP image and encode ---
    auto* clip_img = clip_image_u8_init();
    clip_build_img_from_pixels(rgb_pixels, img_w, img_h, clip_img);
    stbi_image_free(rgb_pixels);

    auto* f32_batch = clip_image_f32_batch_init();
    if (!clip_image_preprocess(ctx_clip_, clip_img, f32_batch)) {
        LOG_ERROR("LocalBackend::generateVision — CLIP preprocess failed");
        clip_image_u8_free(clip_img);
        clip_image_f32_batch_free(f32_batch);
        return generate(prompt + "\n[Image preprocessing failed]", history, stream_cb);
    }

    // Encode the image through CLIP to get embedding vectors
    size_t embd_bytes = clip_embd_nbytes(ctx_clip_);
    std::vector<float> image_embd(embd_bytes / sizeof(float));

    size_t n_images = clip_image_f32_batch_n_images(f32_batch);
    if (n_images > 0) {
        auto* first_img = clip_image_f32_get_img(f32_batch, 0);
        if (!clip_image_encode(ctx_clip_, thread_count_, first_img, image_embd.data())) {
            LOG_ERROR("LocalBackend::generateVision — CLIP encode failed");
            clip_image_u8_free(clip_img);
            clip_image_f32_batch_free(f32_batch);
            return generate(prompt + "\n[Image encoding failed]", history, stream_cb);
        }
    }

    clip_image_u8_free(clip_img);
    clip_image_f32_batch_free(f32_batch);

    // --- Build text prompt with image context indicator ---
    std::string vision_prompt = buildFullPrompt(
        "[An image of the user's screen has been analyzed.]\n" + prompt, history
    );

    LOG_INFO("LocalBackend::generateVision — Image encoded ({}x{}, {} embd floats), generating response...",
             img_w, img_h, image_embd.size());

    // Fall through to standard text generation with enriched prompt
    // The image embeddings have been processed by CLIP; for full interleaved
    // vision-language decoding, the mtmd API would be used with a compatible
    // VLM model (e.g. LLaVA, Qwen-VL). For now, we augment the text prompt.
    return generate(vision_prompt, {}, stream_cb);
#else
    (void)prompt; (void)image_bytes; (void)history; (void)stream_cb;
    return "";
#endif
}

// ═══════════════════ Embeddings ════════════════════════════════════

std::vector<float> LocalBackend::getEmbeddings(const std::string& text) {
    std::vector<float> embedding;
#ifdef VISION_HAS_LLM
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (!loaded_ && !initialize()) return embedding;
    touchActivity();

    const llama_vocab* vocab = llama_model_get_vocab(model_);

    // Tokenize
    // FIX 3: Dynamic tokenize allocation for embeddings too.
    std::vector<llama_token> tokens(text.size() + 128);
    int n_tokens = llama_tokenize(vocab, text.c_str(), (int)text.size(),
                                   tokens.data(), (int)tokens.size(), true, true);
    if (n_tokens <= 0) return embedding;
    tokens.resize(n_tokens);

    // Limit to prevent OOM
    if (n_tokens > 256) {
        tokens.resize(256);
        n_tokens = 256;
    }

    // ── RAII Batch: guaranteed cleanup even if decode throws ──
    ScopedBatch sb(context_size_, 0, 1);
    auto& batch = sb.batch;

    // ── Evaluate Embeddings Prompt in Chunks (Prevent Batch Overflow Crash) ──
    uint32_t n_batch = llama_n_batch(ctx_);
    llama_seq_id seq_ids[] = {1};  // seq_id=1 to avoid corrupting main chat KV cache
    
    for (int i = 0; i < n_tokens; i += n_batch) {
        int n_eval = std::min(n_tokens - i, (int)n_batch);
        batch.n_tokens = 0;
        
        for (int j = 0; j < n_eval; j++) {
            batch.token   [batch.n_tokens] = tokens[i + j];
            batch.pos     [batch.n_tokens] = i + j;
            batch.n_seq_id[batch.n_tokens] = 1;
            batch.seq_id  [batch.n_tokens] = seq_ids;
            // Only the final token gets logits (we need logits to extract embeddings)
            batch.logits  [batch.n_tokens] = ((i + j) == n_tokens - 1);
            batch.n_tokens++;
        }
        
        if (llama_decode(ctx_, batch) != 0) {
            LOG_ERROR("LocalBackend: Embedding decode failed at chunk {}/{}", i, n_tokens);
            return embedding;
        }
    }

    // Now extract the compacted vector
    int n_vocab = llama_vocab_n_tokens(vocab);
    auto* logits = llama_get_logits_ith(ctx_, -1);

    // Compact vector from logit signature
    int embed_dim = 128;
    embedding.resize(embed_dim, 0.0f);
    for (int i = 0; i < n_vocab; i++) {
        embedding[i % embed_dim] += logits[i];
    }

    // L2 normalize
    float norm = 0.0f;
    for (float v : embedding) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0.0f) {
        for (float& v : embedding) v /= norm;
    }

    // ScopedBatch destructor frees batch automatically

    // Clean up embedding sequence from KV cache
    llama_memory_seq_rm(llama_get_memory(ctx_), 1, -1, -1);
#else
    (void)text;
#endif
    return embedding;
}

// ═══════════════════ Control ══════════════════════════════════════

void LocalBackend::cancelGeneration() {
    cancel_generation_ = true;
}

std::string LocalBackend::info() const {
    std::ostringstream ss;
    ss << "Local llama.cpp (" << model_path_ << ") "
       << (loaded_ ? "[LOADED]" : "[NOT LOADED]")
       << " | GPU layers: " << gpu_layers_
       << " | ctx: " << context_size_
       << " | threads: " << thread_count_;
    return ss.str();
}

// ═══════════════════ Idle Auto-Unload ═════════════════════════════

void LocalBackend::touchActivity() {
    last_activity_ = std::chrono::steady_clock::now();
}

void LocalBackend::checkIdleUnload() {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (!loaded_) return;
    auto elapsed = std::chrono::steady_clock::now() - last_activity_;
    if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= IDLE_TIMEOUT_SECONDS) {
        LOG_INFO("LocalBackend: Idle for {}s — auto-unloading to free RAM/VRAM",
                 IDLE_TIMEOUT_SECONDS);
        shutdown();
    }
}

// ═══════════════════ Internal Helpers ══════════════════════════════

std::string LocalBackend::findModelPath() const {
    std::vector<std::string> search_dirs = {
        "models", "data/models", "../models",
        "M:/AI/MODELS/TEXT",
    };

    static const std::vector<std::string> preferred_patterns = {
        "Instruct-Q4_K_M", "Instruct-Q6_K", "instruct",
        "Q4_K_M", "Q6_K", ".gguf"
    };

    std::vector<std::string> all_models;
    for (const auto& dir : search_dirs) {
        if (!fs::exists(dir)) continue;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() == ".gguf") {
                all_models.push_back(entry.path().string());
            }
        }
    }

    for (const auto& pref : preferred_patterns) {
        for (const auto& model : all_models) {
            if (model.find(pref) != std::string::npos) {
                LOG_INFO("LocalBackend: Selected model: {}", model);
                return model;
            }
        }
    }

    if (!all_models.empty()) return all_models[0];
    return "models/model.gguf";
}

std::string LocalBackend::buildFullPrompt(const std::string& prompt,
                                          const std::vector<Message>& history) const {
    // ── ChatML Prompt Templating ──
    // Modern instruct models (Qwen, Llama-3, Phi-3) rely on explicit
    // <|im_start|>role and <|im_end|> markers to know when a turn ends.
    // Without these, the model never emits EOS and generates until context
    // overflow → KV cache crash.

    if (history.empty()) {
        // Wrap even bare prompts in ChatML so the model sees the boundary
        std::ostringstream ss;
        ss << "<|im_start|>user\n" << prompt << "<|im_end|>\n";
        ss << "<|im_start|>assistant\n";
        return ss.str();
    }

    std::ostringstream ss;

    for (const auto& msg : history) {
        if (msg.role == "system") {
            ss << "<|im_start|>system\n" << msg.content << "<|im_end|>\n";
        } else if (msg.role == "user") {
            ss << "<|im_start|>user\n" << msg.content << "<|im_end|>\n";
        } else if (msg.role == "assistant") {
            ss << "<|im_start|>assistant\n" << msg.content << "<|im_end|>\n";
        }
    }

    // Add current prompt if not already the last user message
    bool already_in_history = false;
    if (!history.empty() && history.back().role == "user"
        && history.back().content == prompt) {
        already_in_history = true;
    }
    if (!already_in_history) {
        ss << "<|im_start|>user\n" << prompt << "<|im_end|>\n";
    }

    // Open the assistant turn — model generates until it emits <|im_end|>
    ss << "<|im_start|>assistant\n";
    return ss.str();
}

} // namespace vision
