/**
 * @file instruction_translator.cpp
 * @brief Model-Aware Instruction Translator
 *
 * Translates system personas and prompts between different LLM instruction
 * formats when switching backends. Each model family has quirks:
 *
 * Llama-3:  Verbose, explicit constraints, bullet-point rules, "You MUST/NEVER"
 * Qwen:     Concise imperative, short directives, responds well to structured format
 * Mistral:  Middle ground, benefits from [INST]/[/INST] anchors
 * Phi:      Short, direct, task-focused instructions
 * Gemma:    Conversational tone, clear explicit role definition
 */

#include "instruction_translator.h"
#include <algorithm>
#include <sstream>
#include <regex>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#endif

namespace vision {

// ═══════════════════ Model Family Detection ═══════════════════════

ModelFamily InstructionTranslator::detectFamily(const std::string& model_identifier) {
    std::string lower = model_identifier;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Llama family detection
    if (lower.find("llama") != std::string::npos ||
        lower.find("llama-3") != std::string::npos ||
        lower.find("llama3") != std::string::npos ||
        lower.find("meta-llama") != std::string::npos) {
        return ModelFamily::Llama3;
    }

    // Qwen family detection
    if (lower.find("qwen") != std::string::npos ||
        lower.find("qwen2") != std::string::npos ||
        lower.find("qwen1.5") != std::string::npos) {
        return ModelFamily::Qwen;
    }

    // Mistral family detection
    if (lower.find("mistral") != std::string::npos ||
        lower.find("mixtral") != std::string::npos) {
        return ModelFamily::Mistral;
    }

    // Phi family detection
    if (lower.find("phi-") != std::string::npos ||
        lower.find("phi3") != std::string::npos ||
        lower.find("phi4") != std::string::npos) {
        return ModelFamily::Phi;
    }

    // Gemma family detection
    if (lower.find("gemma") != std::string::npos) {
        return ModelFamily::Gemma;
    }

    return ModelFamily::Generic;
}

// ═══════════════════ System Prompt Translation ════════════════════

std::string InstructionTranslator::translateSystemPrompt(
    const std::string& prompt,
    ModelFamily source,
    ModelFamily target) const {

    // No translation needed if same family
    if (source == target) return prompt;

    LOG_INFO("InstructionTranslator: Translating persona from {} to {}",
             static_cast<int>(source), static_cast<int>(target));

    // Step 1: Adapt verbosity for target model
    std::string adapted = adaptVerbosity(prompt, target);

    // Step 2: Rewrite constraint format
    adapted = adaptConstraints(adapted, target);

    // Step 3: Add model-specific anchors
    adapted = addModelAnchors(adapted, target);

    return adapted;
}

// ═══════════════════ Conversation Translation ════════════════════

std::vector<Message> InstructionTranslator::translateConversation(
    const std::vector<Message>& conversation,
    ModelFamily source,
    ModelFamily target) const {

    if (source == target) return conversation;

    std::vector<Message> translated;
    translated.reserve(conversation.size());

    for (const auto& msg : conversation) {
        if (msg.role == "system") {
            // System prompts get full translation
            translated.emplace_back("system",
                translateSystemPrompt(msg.content, source, target));
        } else {
            // User and assistant messages pass through unchanged
            // (the model should understand natural language regardless)
            translated.push_back(msg);
        }
    }

    return translated;
}

// ═══════════════════ Persona Frames ══════════════════════════════

std::string InstructionTranslator::getPersonaFrame(ModelFamily family) {
    switch (family) {
    case ModelFamily::Llama3:
        // Llama-3 responds best to explicit, detailed role definitions
        // with clear "You MUST" / "You MUST NOT" constraints
        return
            "You are a high-performance Windows system AI agent. "
            "You operate as a native OS controller with direct access to system functions.\n\n"
            "CRITICAL RULES:\n"
            "- You MUST respond with ONLY valid JSON objects.\n"
            "- You MUST NOT include explanations, markdown, or natural language outside JSON.\n"
            "- You MUST NOT hallucinate capabilities you don't have.\n"
            "- You have NO internet access when using local mode.\n"
            "- You strictly execute OS-level commands: open apps, manage windows, "
            "control system settings, file operations.\n\n"
            "RESPONSE FORMAT: {\"thought\": \"reasoning\", \"action\": \"action_name\", "
            "\"params\": {\"key\": \"value\"}}";

    case ModelFamily::Qwen:
        // Qwen prefers concise, imperative instructions with structured format
        // Less verbose than Llama-3 — too many rules confuse Qwen
        return
            "You are a Windows AI system agent. Output JSON commands only.\n"
            "No chat. No explanations. No markdown.\n\n"
            "Format: {\"thought\": \"...\", \"action\": \"...\", \"params\": {...}}\n\n"
            "Available actions: open_app, close_app, type_text, press_key, "
            "search_web, set_volume, set_brightness, task_complete.\n"
            "Respond with one JSON object per turn.";

    case ModelFamily::Mistral:
        // Mistral works well with a balanced, structured approach
        return
            "You are a Windows system AI agent that executes OS commands.\n"
            "Always respond with a single JSON object.\n"
            "Never include text outside the JSON.\n\n"
            "JSON format:\n"
            "{\"thought\": \"your reasoning\", \"action\": \"action_name\", "
            "\"params\": {\"key\": \"value\"}}\n\n"
            "Focus on accuracy. Do not guess.";

    case ModelFamily::Phi:
        // Phi models prefer very short, direct task descriptions
        return
            "Windows AI agent. JSON output only.\n"
            "Format: {\"thought\":\"...\",\"action\":\"...\",\"params\":{...}}\n"
            "Execute system commands. No chat.";

    case ModelFamily::Gemma:
        // Gemma works well with conversational but clear instructions
        return
            "You're a Windows system agent that controls the OS via JSON commands.\n\n"
            "Your responses must be valid JSON with this structure:\n"
            "- \"thought\": Brief reasoning about what to do\n"
            "- \"action\": The system action to perform\n"
            "- \"params\": Parameters for the action\n\n"
            "Don't include any text outside the JSON object.";

    case ModelFamily::Generic:
    default:
        // Safe fallback — works reasonably with most models
        return
            "You are a Windows AI assistant. "
            "Respond with a JSON object containing 'thought', 'action', and 'params'. "
            "Do not include any text outside the JSON.";
    }
}

// ═══════════════════ JSON Reinforcement ══════════════════════════

std::string InstructionTranslator::getJsonReinforcement(ModelFamily family) {
    switch (family) {
    case ModelFamily::Llama3:
        // Llama-3 sometimes wraps JSON in markdown — need strong reinforcement
        return "\n\nIMPORTANT: Output RAW JSON only. "
               "Do NOT wrap in ```json``` code blocks. "
               "Do NOT add any text before or after the JSON object.";

    case ModelFamily::Qwen:
        // Qwen is generally good at JSON but sometimes adds trailing text
        return "\nOutput: JSON only. No trailing text.";

    case ModelFamily::Mistral:
        // Mistral occasionally prefixes with "Here is..."
        return "\nRespond ONLY with the JSON object, nothing else.";

    case ModelFamily::Phi:
        // Phi can be verbose — needs brief reinforcement
        return "\nJSON only.";

    case ModelFamily::Gemma:
        return "\nOutput only the JSON object with no additional text.";

    case ModelFamily::Generic:
    default:
        return "\nRespond with ONLY a valid JSON object.";
    }
}

// ═══════════════════ Internal: Verbosity Adaptation ══════════════

std::string InstructionTranslator::adaptVerbosity(
    const std::string& prompt,
    ModelFamily target) const {

    switch (target) {
    case ModelFamily::Llama3: {
        // Llama-3 benefits from MORE detail — expand concise instructions
        // If prompt is very short (< 100 chars), wrap it in a detailed frame
        if (prompt.size() < 100) {
            std::ostringstream ss;
            ss << "SYSTEM INSTRUCTION:\n" << prompt << "\n\n"
               << "ADDITIONAL CONTEXT:\n"
               << "- Process user requests step by step.\n"
               << "- Consider edge cases and error handling.\n"
               << "- Be precise with parameter values.";
            return ss.str();
        }
        return prompt;
    }

    case ModelFamily::Qwen:
    case ModelFamily::Phi: {
        // Qwen and Phi prefer CONCISE instructions — trim verbose prompts
        // Remove excessive whitespace, collapse multi-line rules into single lines
        std::string trimmed = prompt;

        // Collapse multiple newlines into single newline
        std::regex multi_newline(R"(\n{3,})");
        trimmed = std::regex_replace(trimmed, multi_newline, "\n\n");

        // Remove overly verbose "IMPORTANT:", "CRITICAL:", etc. prefixes
        // that Llama-3 loves but Qwen finds noisy
        std::regex verbose_prefix(R"((?:IMPORTANT|CRITICAL|NOTE|WARNING):\s*)");
        trimmed = std::regex_replace(trimmed, verbose_prefix, "");

        // Collapse "You MUST" / "You MUST NOT" into shorter directives
        std::regex must_pattern(R"(You MUST NOT\s+)");
        trimmed = std::regex_replace(trimmed, must_pattern, "Don't ");
        std::regex must2_pattern(R"(You MUST\s+)");
        trimmed = std::regex_replace(trimmed, must2_pattern, "");

        return trimmed;
    }

    default:
        return prompt;
    }
}

// ═══════════════════ Internal: Constraint Format ═════════════════

std::string InstructionTranslator::adaptConstraints(
    const std::string& prompt,
    ModelFamily target) const {

    switch (target) {
    case ModelFamily::Llama3: {
        // Convert inline constraints to bullet-point list format
        // Llama-3 follows bullet lists much better than inline rules
        std::string adapted = prompt;

        // Look for comma-separated constraint lists and bulletize them
        // e.g. "No chat. No explanations. No markdown." →
        //      "- No chat\n- No explanations\n- No markdown"
        std::regex sentence_list(R"(([A-Z][^.]{5,30}\.)\s+([A-Z][^.]{5,30}\.)\s+([A-Z][^.]{5,30}\.)(?:\s+([A-Z][^.]{5,30}\.))??)");
        std::smatch m;
        if (std::regex_search(adapted, m, sentence_list)) {
            std::ostringstream replacement;
            replacement << "- " << m[1].str();
            replacement << "\n- " << m[2].str();
            replacement << "\n- " << m[3].str();
            if (m[4].matched) {
                replacement << "\n- " << m[4].str();
            }
            adapted = adapted.substr(0, m.position()) +
                      replacement.str() +
                      adapted.substr(m.position() + m.length());
        }
        return adapted;
    }

    case ModelFamily::Qwen: {
        // Convert bullet-point lists into compact inline format
        // Qwen processes inline rules faster and more reliably
        std::string adapted = prompt;

        // Convert "- Rule\n- Rule\n" into "Rule. Rule. "
        std::regex bullet_pattern(R"(\n?-\s+([^\n]+))");
        adapted = std::regex_replace(adapted, bullet_pattern, " $1.");

        // Clean up double periods
        std::regex double_period(R"(\.\.)");
        adapted = std::regex_replace(adapted, double_period, ".");

        return adapted;
    }

    default:
        return prompt;
    }
}

// ═══════════════════ Internal: Model-Specific Anchors ════════════

std::string InstructionTranslator::addModelAnchors(
    const std::string& prompt,
    ModelFamily target) const {

    std::ostringstream ss;

    switch (target) {
    case ModelFamily::Llama3:
        // Llama-3 follows clear section headers
        if (prompt.find("RULES:") == std::string::npos &&
            prompt.find("INSTRUCTIONS:") == std::string::npos) {
            ss << "## INSTRUCTIONS\n\n" << prompt;
        } else {
            ss << prompt;
        }
        // Add JSON reinforcement at the end
        ss << getJsonReinforcement(ModelFamily::Llama3);
        break;

    case ModelFamily::Qwen:
        // Qwen follows direct instruction blocks
        ss << prompt;
        ss << getJsonReinforcement(ModelFamily::Qwen);
        break;

    case ModelFamily::Mistral:
        // Mistral benefits from [INST] anchors (even in system prompt)
        ss << prompt;
        ss << getJsonReinforcement(ModelFamily::Mistral);
        break;

    case ModelFamily::Phi:
        ss << prompt;
        ss << getJsonReinforcement(ModelFamily::Phi);
        break;

    case ModelFamily::Gemma:
        ss << prompt;
        ss << getJsonReinforcement(ModelFamily::Gemma);
        break;

    default:
        ss << prompt;
        ss << getJsonReinforcement(ModelFamily::Generic);
        break;
    }

    return ss.str();
}

} // namespace vision
