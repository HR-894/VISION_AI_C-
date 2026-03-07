/**
 * @file confidence_scorer.cpp
 * @brief Deterministic Confidence Scoring + HITL State Machine
 *
 * Confidence is NEVER based on LLM self-rating (LLMs are confident liars).
 * Instead, it uses 4 deterministic signals:
 *   1. Memory similarity (have we seen this before?)
 *   2. Context match (is the target actually available?)
 *   3. Template match score (does it fit a known pattern?)
 *   4. Safety override (dangerous keywords → hard cap 0%)
 *
 * HITL Flow:
 *   Low confidence → PendingAction → user replies → tryResolve()
 *   Bailout if: "cancel", unrelated command, or 60s timeout
 */

#include "confidence_scorer.h"
#include "vector_memory.h"
#include <algorithm>
#include <sstream>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define LOG_WARN(...)  spdlog::warn(__VA_ARGS__)
#else
#define LOG_INFO(...)  (void)0
#define LOG_WARN(...)  (void)0
#endif

namespace vision {

// ═══════════════════ Safety & Bailout Keywords ══════════════════════

const std::unordered_set<std::string> ConfidenceScorer::kDangerousKeywords = {
    "format", "delete", "remove", "erase", "kill", "terminate",
    "shutdown", "restart", "reboot", "wipe", "uninstall", "rmdir",
    "rd /s", "del /f", "rm -rf", "fdisk", "diskpart", "reg delete",
    "system32", "registry", "bios", "firmware"
};

const std::unordered_set<std::string> ConfidenceScorer::kBailoutKeywords = {
    "cancel", "abort", "stop", "nevermind", "never mind", "nvm",
    "forget it", "skip", "nah", "no", "nahi", "rehne do", "chhod do",
    "leave it", "ruk", "mat kar", "band kar"
};

// ═══════════════════ Constructor ═════════════════════════════════════

ConfidenceScorer::ConfidenceScorer() = default;

// ═══════════════════ Scoring ════════════════════════════════════════

bool ConfidenceScorer::isDangerous(const std::string& command) const {
    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const auto& keyword : kDangerousKeywords) {
        if (lower.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool ConfidenceScorer::isBailout(const std::string& input) const {
    std::string lower = input;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // Strip whitespace
    while (!lower.empty() && lower.back() == ' ') lower.pop_back();
    while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());

    // Exact match bailout keywords
    if (kBailoutKeywords.count(lower)) return true;

    // Check if input contains a bailout keyword as a standalone word
    for (const auto& kw : kBailoutKeywords) {
        if (lower.find(kw) != std::string::npos) return true;
    }
    return false;
}

ConfidenceResult ConfidenceScorer::score(const std::string& command) const {
    ConfidenceResult result;
    result.score = kBaseline;  // 0.10 baseline (benefit of the doubt)

    // ── Signal 1: Memory Similarity ──────────────────────────────
    // "Have we successfully done something like this before?"
    if (vector_memory_ && vector_memory_->size() > 0) {
        auto matches = vector_memory_->search(command, 1, 0.3f);
        if (!matches.empty()) {
            result.memory_signal = matches[0].similarity;
            result.score += result.memory_signal * kWeightMemory;
        }
    }

    // ── Signal 2: Context Match ──────────────────────────────────
    // "Is the target actually available/running?"
    if (app_list_fn_) {
        auto apps = app_list_fn_();
        std::string lower = command;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        int context_matches = 0;
        for (const auto& app : apps) {
            std::string lower_app = app;
            std::transform(lower_app.begin(), lower_app.end(),
                           lower_app.begin(), ::tolower);
            if (lower.find(lower_app) != std::string::npos) {
                context_matches++;
            }
        }

        if (context_matches > 0) {
            result.context_signal = 1.0f;
            result.score += kWeightContext;

            // Ambiguity penalty: multiple matches = which one?
            if (context_matches > 1) {
                result.ambiguity_penalty = 1.0f / context_matches;
                result.reason = "Multiple targets found (" +
                                std::to_string(context_matches) + " matches)";
            }
        } else {
            // Check if command seems to reference a specific app
            // but it's not running → score stays low (no context boost)
            bool references_app = (lower.find("close") != std::string::npos ||
                                    lower.find("switch") != std::string::npos ||
                                    lower.find("focus") != std::string::npos);
            if (references_app) {
                result.context_signal = 0.0f;
                result.reason = "Target app not found in running processes";
            }
        }
    }

    // ── Signal 3: Template Match Score ────────────────────────────
    // "Does this fit a known command pattern?"
    if (template_score_fn_) {
        result.template_signal = template_score_fn_(command);
        result.score += result.template_signal * kWeightTemplate;
    }

    // Apply ambiguity penalty
    result.score *= result.ambiguity_penalty;

    // ── Signal 4: Safety Override (HARD CAP) ─────────────────────
    if (isDangerous(command)) {
        result.safety_blocked = true;
        result.requires_confirmation = true;
        result.score = 0.0f;
        result.level = ConfidenceLevel::Blocked;
        result.reason = "⚠️ Dangerous operation detected — requires explicit confirmation";
        LOG_WARN("Safety override: '{}' blocked by confidence scorer", command);
        return result;
    }

    // ── Classify confidence level ────────────────────────────────
    result.score = std::clamp(result.score, 0.0f, 1.0f);

    if (result.score >= kHighThreshold) {
        result.level = ConfidenceLevel::High;
        result.requires_confirmation = false;
    } else if (result.score >= kMediumThreshold) {
        result.level = ConfidenceLevel::Medium;
        result.requires_confirmation = false;
        if (result.reason.empty()) {
            result.reason = "Moderate confidence — executing with disclaimer";
        }
    } else if (result.score >= kLowThreshold) {
        result.level = ConfidenceLevel::Low;
        result.requires_confirmation = true;
        if (result.reason.empty()) {
            result.reason = "Low confidence — asking for confirmation";
        }
    } else {
        result.level = ConfidenceLevel::Blocked;
        result.requires_confirmation = true;
        if (result.reason.empty()) {
            result.reason = "Very low confidence — requires clarification";
        }
    }

    LOG_INFO("Confidence: {:.0f}% [mem={:.2f} ctx={:.2f} tpl={:.2f} ambig={:.2f}] {}",
             result.score * 100, result.memory_signal, result.context_signal,
             result.template_signal, result.ambiguity_penalty, result.reason);

    return result;
}

bool ConfidenceScorer::shouldAutoExecute(const ConfidenceResult& result) const {
    return !result.requires_confirmation &&
           (result.level == ConfidenceLevel::High ||
            result.level == ConfidenceLevel::Medium);
}

// ═══════════════════ HITL State Machine ═════════════════════════════

bool ConfidenceScorer::hasPending() const {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    return pending_.has_value() && !pending_->isExpired();
}

std::optional<PendingAction> ConfidenceScorer::getPending() const {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_ && !pending_->isExpired()) {
        return *pending_;  // Thread-safe copy
    }
    return std::nullopt;
}

void ConfidenceScorer::setPending(PendingAction action) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    action.created = std::chrono::steady_clock::now();
    pending_ = std::move(action);
    LOG_INFO("HITL: Pending action set — '{}'", pending_->original_command);
}

void ConfidenceScorer::clearPending() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_) {
        LOG_INFO("HITL: Pending action cleared — '{}'", pending_->original_command);
    }
    pending_.reset();
}

ConfidenceScorer::ResolveOutput ConfidenceScorer::tryResolve(const std::string& user_input) {
    std::lock_guard<std::mutex> lock(pending_mutex_);

    // No pending action
    if (!pending_) {
        return {ResolveResult::NoPending, ""};
    }

    // Expired check
    if (pending_->isExpired()) {
        LOG_INFO("HITL: Pending action expired — '{}'", pending_->original_command);
        pending_.reset();
        return {ResolveResult::NoPending, ""};
    }

    // ── Bailout detection ────────────────────────────────────────
    // Check BEFORE option matching — "cancel" should always work
    // even if "cancel" happens to be an option name
    if (isBailout(user_input)) {
        LOG_INFO("HITL: User bailed out of '{}'", pending_->original_command);
        pending_.reset();
        return {ResolveResult::Bailout, ""};
    }

    // ── Option matching ──────────────────────────────────────────
    std::string lower_input = user_input;
    std::transform(lower_input.begin(), lower_input.end(),
                   lower_input.begin(), ::tolower);
    // Strip whitespace
    while (!lower_input.empty() && lower_input.back() == ' ') lower_input.pop_back();
    while (!lower_input.empty() && lower_input.front() == ' ')
        lower_input.erase(lower_input.begin());

    for (const auto& option : pending_->options) {
        std::string lower_option = option;
        std::transform(lower_option.begin(), lower_option.end(),
                       lower_option.begin(), ::tolower);

        // Exact match or substring match
        if (lower_input == lower_option ||
            lower_input.find(lower_option) != std::string::npos ||
            lower_option.find(lower_input) != std::string::npos) {

            // Build resolved command
            std::string resolved = pending_->resolved_command;
            // Replace placeholder {OPTION} with matched option
            size_t pos = resolved.find("{OPTION}");
            if (pos != std::string::npos) {
                resolved.replace(pos, 8, option);
            } else {
                // Fallback: original command + option
                resolved = pending_->original_command + " " + option;
            }

            LOG_INFO("HITL: Resolved '{}' → '{}'", pending_->original_command, resolved);
            pending_.reset();
            return {ResolveResult::Matched, resolved};
        }
    }

    // ── No match — check if it looks like a completely new command ──
    // Heuristic: if input is long (>3 words) or matches common command patterns,
    // treat as bailout (new command) rather than invalid option
    int word_count = 0;
    {
        std::istringstream iss(user_input);
        std::string w;
        while (iss >> w) word_count++;
    }

    if (word_count >= 3) {
        // Probably a new command, not trying to answer the question
        LOG_INFO("HITL: Input '{}' looks like new command — bailing out",
                 user_input);
        pending_.reset();
        return {ResolveResult::Bailout, ""};
    }

    // Input doesn't match any option — ask again
    return {ResolveResult::NoMatch, ""};
}

} // namespace vision
