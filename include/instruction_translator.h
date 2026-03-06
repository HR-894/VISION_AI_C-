#pragma once
/**
 * @file instruction_translator.h
 * @brief Model-Aware Instruction Translator for Dual-Inference Engine
 *
 * When switching backends, the system prompt / persona must be adapted
 * to the target model's instruction format. Different model families
 * parse instructions differently:
 *
 *   - Llama-3 (Groq Cloud): Prefers explicit, structured role instructions
 *     with bullet-point constraints. Follows "do/don't" lists well.
 *
 *   - Qwen (Local GGUF): Prefers concise, imperative instructions with
 *     ChatML-style formatting. Follows short directives better.
 *
 * The InstructionTranslator sits between LLMController and the backends,
 * automatically rewriting the system persona and conversation messages
 * to optimize instruction-following for the target model.
 */

#include "ai_backend.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace vision {

/// Known model families for instruction optimization
enum class ModelFamily {
    Llama3,      // Meta Llama-3.x (Groq cloud default)
    Qwen,        // Alibaba Qwen-2.x (common local GGUF)
    Mistral,     // Mistral/Mixtral family
    Phi,         // Microsoft Phi family
    Gemma,       // Google Gemma family
    Generic      // Unknown model — use neutral format
};

/**
 * @class InstructionTranslator
 * @brief Translates system personas and prompts between model instruction formats
 *
 * Usage:
 *   InstructionTranslator translator;
 *   translator.setSourceFamily(ModelFamily::Qwen);    // current backend's model
 *   translator.setTargetFamily(ModelFamily::Llama3);   // new backend's model
 *
 *   // Translate system prompt for new model
 *   std::string adapted = translator.translateSystemPrompt(original_prompt);
 *
 *   // Translate entire conversation history
 *   auto adapted_history = translator.translateConversation(conversation);
 */
class InstructionTranslator {
public:
    InstructionTranslator() = default;

    // ── Model Family Detection ───────────────────────────────────

    /// Auto-detect model family from model name/path string
    /// e.g. "llama-3.3-70b-versatile" → Llama3, "Qwen2.5-7B-Instruct" → Qwen
    static ModelFamily detectFamily(const std::string& model_identifier);

    // ── Translation ──────────────────────────────────────────────

    /// Translate a system prompt from source model format to target model format
    /// @param prompt       The original system prompt
    /// @param source       The model family the prompt was written for
    /// @param target       The model family to adapt the prompt to
    /// @return Adapted system prompt optimized for the target model
    std::string translateSystemPrompt(const std::string& prompt,
                                      ModelFamily source,
                                      ModelFamily target) const;

    /// Translate an entire conversation history for backend switch
    /// Adapts system messages and optionally reformats user/assistant messages
    /// @param conversation  Original conversation
    /// @param source        Source model family
    /// @param target        Target model family
    /// @return Adapted conversation vector
    std::vector<Message> translateConversation(const std::vector<Message>& conversation,
                                               ModelFamily source,
                                               ModelFamily target) const;

    /// Get the recommended persona wrapper for a given model family
    /// This is the "frame" around the core instruction that helps
    /// the model understand its role
    static std::string getPersonaFrame(ModelFamily family);

    /// Get the recommended JSON instruction reinforcement for a model family
    /// Some models need stronger nudging to output valid JSON
    static std::string getJsonReinforcement(ModelFamily family);

private:
    // ── Core Translation Rules ───────────────────────────────────

    /// Adapt prompt style: verbose ↔ concise depending on model preference
    std::string adaptVerbosity(const std::string& prompt,
                               ModelFamily target) const;

    /// Rewrite constraint format: bullet lists vs inline directives
    std::string adaptConstraints(const std::string& prompt,
                                 ModelFamily target) const;

    /// Add model-specific instruction anchors
    /// (e.g., Qwen responds better to Chinese-style 要求/规则 anchors in edge cases)
    std::string addModelAnchors(const std::string& prompt,
                                ModelFamily target) const;
};

} // namespace vision
