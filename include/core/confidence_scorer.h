#pragma once
/**
 * @file confidence_scorer.h
 * @brief Agent Confidence Scoring + Human-in-the-Loop (HITL) State Machine
 *
 * Architecture:
 *   Deterministic Hybrid Confidence (NO LLM self-rating!)
 *   ┌─────────────────────────────────────────────────────┐
 *   │ Signal 1: Memory Similarity  (0-1.0) × 0.30       │
 *   │ Signal 2: Context Match      (0/1.0) × 0.25       │
 *   │ Signal 3: Template Match     (0-1.0) × 0.35       │
 *   │ Signal 4: Safety Override    (hard cap → 0%)       │
 *   │ Ambiguity Penalty: score /= num_matches            │
 *   └─────────────────────────────────────────────────────┘
 *
 *   HITL: PendingAction state machine (zero thread blocking)
 *   - Low confidence → ask user → resume on reply
 *   - Bailout detection ("cancel", unrelated command)
 *   - 60s timeout via QTimer
 *   - std::mutex for thread safety
 */

#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <chrono>
#include <functional>
#include <unordered_set>

namespace vision {

// Forward declarations
class VectorMemory;

// ═══════════════════ Confidence Result ═══════════════════════════════

enum class ConfidenceLevel {
    High,           // ≥ 0.75 — execute immediately
    Medium,         // 0.50-0.74 — execute with disclaimer
    Low,            // 0.25-0.49 — ask for confirmation
    Blocked         // < 0.25 or safety override — REQUIRES human approval
};

struct ConfidenceResult {
    float score = 0.0f;                 // 0.0 - 1.0
    ConfidenceLevel level = ConfidenceLevel::Blocked;
    std::string reason;                 // Human-readable explanation
    bool requires_confirmation = false;
    bool safety_blocked = false;

    // Breakdown for debugging/logging
    float memory_signal = 0.0f;
    float context_signal = 0.0f;
    float template_signal = 0.0f;
    float ambiguity_penalty = 1.0f;     // 1.0 = no penalty, 0.5 = 2 matches, etc.
};

// ═══════════════════ Pending Action (HITL) ═══════════════════════════

struct PendingAction {
    std::string original_command;
    std::string clarification_question;
    std::vector<std::string> options;       // Valid responses
    std::string resolved_command;           // Template for resolved command
    ConfidenceResult confidence;
    std::chrono::steady_clock::time_point created;

    bool isExpired(int timeout_seconds = 60) const {
        auto age = std::chrono::steady_clock::now() - created;
        return std::chrono::duration_cast<std::chrono::seconds>(age).count() > timeout_seconds;
    }
};

// ═══════════════════ Confidence Scorer ═══════════════════════════════

class ConfidenceScorer {
public:
    /// Callback types for context signals
    using AppListFn = std::function<std::vector<std::string>()>;
    using TemplateScoreFn = std::function<float(const std::string&)>;

    ConfidenceScorer();

    /// Set the vector memory for similarity scoring
    void setVectorMemory(VectorMemory* vm) { vector_memory_ = vm; }

    /// Set callback to get list of running apps
    void setAppListFn(AppListFn fn) { app_list_fn_ = std::move(fn); }

    /// Set callback to get template match score
    void setTemplateScoreFn(TemplateScoreFn fn) { template_score_fn_ = std::move(fn); }

    // ── Scoring ────────────────────────────────────────────────────

    /// Compute confidence for a command
    ConfidenceResult score(const std::string& command) const;

    /// Should this command be executed without asking?
    bool shouldAutoExecute(const ConfidenceResult& result) const;

    // ── HITL State Machine ─────────────────────────────────────────

    /// Check if there's a pending action awaiting user response
    bool hasPending() const;

    /// Get the pending action (thread-safe copy)
    std::optional<PendingAction> getPending() const;

    /// Set a new pending action (called from worker thread)
    void setPending(PendingAction action);

    /// Try to resolve pending action with user input
    /// Returns: resolved command if matched, nullopt if not
    /// Side effect: clears pending_ on match OR bailout
    enum class ResolveResult { Matched, Bailout, NoMatch, NoPending };

    struct ResolveOutput {
        ResolveResult result;
        std::string resolved_command;   // Only valid if Matched
    };

    ResolveOutput tryResolve(const std::string& user_input);

    /// Clear pending action (timeout or explicit cancel)
    void clearPending();

private:
    VectorMemory* vector_memory_ = nullptr;  // Non-owning
    AppListFn app_list_fn_;
    TemplateScoreFn template_score_fn_;

    // ── HITL State ─────────────────────────────────────────────────
    mutable std::mutex pending_mutex_;
    std::optional<PendingAction> pending_;

    // ── Safety ─────────────────────────────────────────────────────
    static const std::unordered_set<std::string> kDangerousKeywords;
    static const std::unordered_set<std::string> kBailoutKeywords;

    bool isDangerous(const std::string& command) const;
    bool isBailout(const std::string& input) const;

    // ── Signal weights ─────────────────────────────────────────────
    static constexpr float kWeightMemory   = 0.30f;
    static constexpr float kWeightContext  = 0.25f;
    static constexpr float kWeightTemplate = 0.35f;
    // Remaining 0.10 = baseline (benefit of the doubt)
    static constexpr float kBaseline = 0.10f;

    static constexpr float kHighThreshold   = 0.75f;
    static constexpr float kMediumThreshold = 0.50f;
    static constexpr float kLowThreshold    = 0.25f;
};

} // namespace vision
