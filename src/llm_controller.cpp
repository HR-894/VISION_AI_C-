/**
 * @file llm_controller.cpp
 * @brief llama.cpp LLM inference with KV cache shifting, idle unload,
 *        cancel generation, embeddings, and anti-hallucination fallback
 */

#include "llm_controller.h"
#include <filesystem>
#include <functional>
#include <regex>
#include <sstream>
#include <thread>
#include <cmath>
#include <algorithm>

#ifdef VISION_HAS_LLM
#include "llama.h"
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
using json = nlohmann::json;

namespace vision {

LLMController::LLMController(const std::string& model_path)
    : model_path_(model_path), last_activity_(std::chrono::steady_clock::now()) {
    if (model_path_.empty()) model_path_ = findModelPath();
}

LLMController::~LLMController() { unloadModel(); }

bool LLMController::isAvailable() {
#ifdef VISION_HAS_LLM
    return true;
#else
    return false;
#endif
}

std::string LLMController::findModelPath() const {
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
                LOG_INFO("Selected LLM model: {}", model);
                return model;
            }
        }
    }

    if (!all_models.empty()) return all_models[0];
    return "models/model.gguf";
}

void LLMController::touchActivity() {
    last_activity_ = std::chrono::steady_clock::now();
}

void LLMController::checkIdleUnload() {
    if (!loaded_) return;
    auto elapsed = std::chrono::steady_clock::now() - last_activity_;
    if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= IDLE_TIMEOUT_SECONDS) {
        LOG_INFO("LLM idle for {}s — auto-unloading to free RAM/VRAM", IDLE_TIMEOUT_SECONDS);
        unloadModel();
    }
}

bool LLMController::loadModel() {
#ifdef VISION_HAS_LLM
    if (loaded_) return true;

    if (!fs::exists(model_path_)) {
        LOG_ERROR("LLM model not found: {}", model_path_);
        return false;
    }

    llama_backend_init();

    auto model_params = llama_model_default_params();
    model_params.n_gpu_layers = gpu_layers_;

    model_ = llama_model_load_from_file(model_path_.c_str(), model_params);
    if (!model_) {
        LOG_ERROR("Failed to load LLM model: {}", model_path_);
        return false;
    }

    auto ctx_params = llama_context_default_params();
    ctx_params.n_ctx = context_size_;
    // Thread count: use configured value (half of hw threads to prevent OS freeze)
    ctx_params.n_threads = std::max(1, thread_count_);

    ctx_ = llama_init_from_model(model_, ctx_params);
    if (!ctx_) {
        LOG_ERROR("Failed to create LLM context");
        llama_model_free(model_);
        model_ = nullptr;
        return false;
    }

    loaded_ = true;
    touchActivity();
    LOG_INFO("LLM loaded: {} (GPU layers: {}, threads: {}, ctx: {})",
             model_path_, gpu_layers_, thread_count_, context_size_);
    return true;
#else
    return false;
#endif
}

void LLMController::unloadModel() {
#ifdef VISION_HAS_LLM
    if (ctx_) { llama_free(ctx_); ctx_ = nullptr; }
    if (model_) { llama_model_free(model_); model_ = nullptr; }
    loaded_ = false;
#endif
}

bool LLMController::isModelLoaded() const { return loaded_; }

std::string LLMController::generateResponse(const std::string& prompt) {
#ifdef VISION_HAS_LLM
    if (!loaded_ && !loadModel()) return "";

    touchActivity();
    cancel_generation_ = false;

    // Check cache first
    auto cached = getCachedResponse(prompt);
    if (cached) return *cached;

    // Tokenize
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    std::vector<llama_token> tokens(context_size_);
    int n_tokens = llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(),
                                   tokens.data(), (int)tokens.size(), true, true);
    if (n_tokens < 0) {
        LOG_ERROR("Tokenization failed");
        return "";
    }
    tokens.resize(n_tokens);

    // ── KV Cache Shifting: if prompt exceeds half the context, trim ──
    // Keep the first 128 tokens (system prompt) and drop the middle
    if (n_tokens > context_size_ / 2) {
        int keep_prefix = std::min(128, n_tokens / 4);
        int keep_suffix = context_size_ / 2 - keep_prefix;
        std::vector<llama_token> trimmed;
        trimmed.insert(trimmed.end(), tokens.begin(), tokens.begin() + keep_prefix);
        trimmed.insert(trimmed.end(), tokens.end() - keep_suffix, tokens.end());
        tokens = std::move(trimmed);
        n_tokens = (int)tokens.size();
        LOG_WARN("KV cache shift: trimmed prompt from {} to {} tokens (kept system prompt)",
                 n_tokens + keep_suffix, n_tokens);
    }

    // Create batch and decode
    llama_batch batch = llama_batch_init(context_size_, 0, 1);
    batch.n_tokens = 0;
    llama_seq_id seq_ids[] = {0};
    for (int i = 0; i < n_tokens; i++) {
        batch.token   [batch.n_tokens] = tokens[i];
        batch.pos     [batch.n_tokens] = i;
        batch.n_seq_id[batch.n_tokens] = 1;
        batch.seq_id  [batch.n_tokens] = seq_ids;
        batch.logits  [batch.n_tokens] = (i == n_tokens - 1);
        batch.n_tokens++;
    }

    if (llama_decode(ctx_, batch) != 0) {
        LOG_ERROR("LLM decode failed (initial prompt)");
        llama_batch_free(batch);
        return "";
    }

    // Generate tokens
    std::string result;
    int n_cur = n_tokens;

    for (int i = 0; i < max_tokens_; i++) {
        // ── Emergency Stop check ──
        if (cancel_generation_.load()) {
            LOG_WARN("Generation cancelled by user (emergency stop)");
            break;
        }

        auto* logits = llama_get_logits_ith(ctx_, -1);
        int n_vocab_size = llama_vocab_n_tokens(vocab);

        // Simple temperature sampling
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
            // Simple top-p sampling with temperature
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

        // Check for end of generation
        if (llama_vocab_is_eog(vocab, new_token)) break;

        // Convert token to string
        char buf[128];
        int len = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);
        if (len > 0) result.append(buf, len);

        // ── KV cache shifting during generation ──
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

        if (llama_decode(ctx_, batch) != 0) break;
    }

    llama_batch_free(batch);
    cancel_generation_ = false;

    // Cache the response
    cacheResponse(prompt, result);

    return result;
#else
    (void)prompt;
    return "";
#endif
}

std::string LLMController::generateReactResponse(const std::string& prompt) {
    return generateResponse(prompt);
}

// ═══════════════════ Embeddings (Lightweight Vector Memory) ═══════════════════

std::vector<float> LLMController::getEmbeddings(const std::string& text) {
    std::vector<float> embedding;
#ifdef VISION_HAS_LLM
    if (!loaded_ && !loadModel()) return embedding;
    touchActivity();

    const llama_vocab* vocab = llama_model_get_vocab(model_);

    // Tokenize
    std::vector<llama_token> tokens(context_size_);
    int n_tokens = llama_tokenize(vocab, text.c_str(), (int)text.size(),
                                   tokens.data(), (int)tokens.size(), true, true);
    if (n_tokens <= 0) return embedding;
    tokens.resize(n_tokens);

    // Limit to prevent OOM for large texts
    if (n_tokens > 256) {
        tokens.resize(256);
        n_tokens = 256;
    }

    // Build batch
    llama_batch batch = llama_batch_init(context_size_, 0, 1);
    batch.n_tokens = 0;
    llama_seq_id seq_ids[] = {0};
    for (int i = 0; i < n_tokens; i++) {
        batch.token   [batch.n_tokens] = tokens[i];
        batch.pos     [batch.n_tokens] = i;
        batch.n_seq_id[batch.n_tokens] = 1;
        batch.seq_id  [batch.n_tokens] = seq_ids;
        batch.logits  [batch.n_tokens] = (i == n_tokens - 1);
        batch.n_tokens++;
    }

    if (llama_decode(ctx_, batch) == 0) {
        // Get embedding from the last token's logits as a lightweight proxy
        // (True embeddings require embedding model; this uses logit signature)
        int n_vocab = llama_vocab_n_tokens(vocab);
        auto* logits = llama_get_logits_ith(ctx_, -1);

        // Take a fixed-size hash of the logits as a lightweight embedding
        int embed_dim = 128;  // Compact vector
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
    }

    llama_batch_free(batch);
#else
    (void)text;
#endif
    return embedding;
}

// ═══════════════════ ReAct Step ═══════════════════

std::optional<json> LLMController::reactStep(
    const std::string& task,
    const json& observation,
    const std::vector<json>& history) {

    std::string prompt = "You are a Windows AI assistant. "
        "Respond with a JSON object containing 'thought' and 'action'.\n\n"
        "Task: " + task + "\n\n"
        "Current observation:\n" + observation.dump(2) + "\n\n";

    if (!history.empty()) {
        prompt += "Previous actions:\n" + formatHistory(history) + "\n\n";
    }

    prompt += "Respond with ONLY a JSON object:\n"
              "{\"thought\": \"your reasoning\", "
              "\"action\": \"action_name\", "
              "\"params\": {\"key\": \"value\"}}";

    std::string response = generateResponse(prompt);
    if (response.empty()) return std::nullopt;

    try {
        auto parsed = parseJsonStrict(response);
        return validateAndFill(parsed);
    } catch (...) {
        LOG_WARN("Failed to parse ReAct response: {}", response);
        return std::nullopt;
    }
}

std::optional<json> LLMController::parseAmbiguousCommand(
    const std::string& command, const std::string& context) {

    std::string prompt = "Parse this command into a JSON action. Command: \"" + command + "\"";
    if (!context.empty()) prompt += "\nContext: " + context;
    prompt += "\n\nRespond with ONLY: {\"action\": \"name\", \"params\": {}}";

    std::string response = generateResponse(prompt);
    if (response.empty()) return std::nullopt;

    try { return parseJsonStrict(response); }
    catch (...) { return std::nullopt; }
}

// ═══════════════════ Robust JSON Parsing + Anti-Hallucination ═══════════════════

json LLMController::parseJsonStrict(const std::string& text) {
    // Try direct parse
    try { return json::parse(text); }
    catch (...) {}

    // Try to extract JSON from text
    auto start = text.find('{');
    auto end = text.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start) {
        try { return json::parse(text.substr(start, end - start + 1)); }
        catch (...) {}
    }

    // Try to extract from markdown code block
    std::regex json_block(R"(```(?:json)?\s*\n?(\{[\s\S]*?\})\s*\n?```)");
    std::smatch m;
    if (std::regex_search(text, m, json_block)) {
        try { return json::parse(m[1].str()); }
        catch (...) {}
    }

    // ── Anti-Hallucination Fallback ──────────────────────────────
    // If the LLM produced plain text instead of JSON, wrap it into a safe action
    // so the agent loop doesn't crash or get stuck
    LOG_WARN("LLM hallucinated plain text instead of JSON — wrapping as action");

    std::string trimmed = text;
    // Trim whitespace
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

    if (trimmed.empty()) {
        return {{"thought", "No response from LLM"}, {"action", "task_complete"},
                {"params", {{"message", "Unable to process — no output"}}}};
    }

    // If it looks like a direct answer/explanation, wrap as task_complete
    // If it mentions an app or action, wrap as type_text
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("type") != std::string::npos || lower.find("write") != std::string::npos ||
        lower.find("enter") != std::string::npos || trimmed.size() < 50) {
        return {{"thought", "LLM produced text — forwarding as type_text"},
                {"action", "type_text"},
                {"params", {{"text", trimmed}}}};
    }

    return {{"thought", "LLM produced text instead of JSON — completing task"},
            {"action", "task_complete"},
            {"params", {{"message", trimmed}}}};
}

json LLMController::validateAndFill(json parsed) {
    if (!parsed.contains("action")) parsed["action"] = "task_complete";
    if (!parsed.contains("params")) parsed["params"] = json::object();
    if (!parsed.contains("thought")) parsed["thought"] = "";
    return parsed;
}

std::string LLMController::formatHistory(const std::vector<json>& history) {
    std::ostringstream ss;
    for (size_t i = 0; i < history.size(); i++) {
        ss << (i + 1) << ". " << history[i].dump() << "\n";
    }
    return ss.str();
}

size_t LLMController::hashPrompt(const std::string& prompt) const {
    return std::hash<std::string>{}(prompt);
}

std::optional<std::string> LLMController::getCachedResponse(const std::string& prompt) {
    std::lock_guard lock(cache_mutex_);
    auto it = cache_.find(prompt);
    if (it != cache_.end()) {
        auto elapsed = std::chrono::steady_clock::now() - it->second.time;
        if (elapsed < std::chrono::seconds(cache_ttl_seconds_)) {
            return it->second.response;
        }
        cache_.erase(it);
    }
    return std::nullopt;
}

void LLMController::cacheResponse(const std::string& prompt, const std::string& response) {
    std::lock_guard lock(cache_mutex_);
    cache_[prompt] = {response, std::chrono::steady_clock::now()};

    // Limit cache size
    if (cache_.size() > 100) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.time < oldest->second.time) oldest = it;
        }
        cache_.erase(oldest);
    }
}

std::string LLMController::getModelInfo() const {
    return model_path_ + (loaded_ ? " (loaded)" : " (not loaded)") +
           " | GPU layers: " + std::to_string(gpu_layers_) +
           " | ctx: " + std::to_string(context_size_) +
           " | threads: " + std::to_string(thread_count_);
}

} // namespace vision
