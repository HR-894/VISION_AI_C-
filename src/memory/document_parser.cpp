/**
 * @file document_parser.cpp
 * @brief Document parsing and text chunking implementation
 */

#include "document_parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace vision {

bool DocumentParser::isSupported(const std::string& filepath) {
    namespace fs = std::filesystem;
    if (!fs::exists(filepath)) return false;

    std::string ext = fs::path(filepath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return ext == ".txt" || ext == ".md" || ext == ".csv" || ext == ".json";
}

std::string DocumentParser::parseToText(const std::string& filepath) {
    namespace fs = std::filesystem;
    if (!fs::exists(filepath)) {
        LOG_WARN("File not found for parsing: {}", filepath);
        return "";
    }

    std::string ext = fs::path(filepath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".pdf") {
        return parsePdfStub(filepath);
    }

    return parseTxt(filepath);
}

std::string DocumentParser::parseTxt(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::in | std::ios::binary);
    if (!file) {
        LOG_ERROR("Failed to open TXT/MD file: {}", filepath);
        return "";
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string DocumentParser::parsePdfStub(const std::string& filepath) {
    LOG_WARN("PDF parsing requested for {}, but libmupdf is not linked. Returning stub.", filepath);
    return "[PDF content extraction requires libmupdf/poppler plugin. "
           "Please convert the PDF to text or ensure the PDF plugin is loaded.]";
}

std::vector<DocumentChunk> DocumentParser::chunkText(const std::string& text,
                                                     int max_tokens,
                                                     int overlap_tokens) {
    std::vector<DocumentChunk> chunks;
    if (text.empty()) return chunks;

    // Heuristic: 1 token ≈ 4 characters in English
    int max_chars = max_tokens * 4;
    int overlap_chars = overlap_tokens * 4;
    if (overlap_chars >= max_chars) overlap_chars = max_chars / 2;

    int step = max_chars - overlap_chars;
    int length = static_cast<int>(text.length());
    int index = 0;

    for (int start = 0; start < length; start += step) {
        int end = std::min(start + max_chars, length);
        
        // Try to snap 'end' to the nearest whitespace or punctuation to avoid cutting words
        if (end < length) {
            int lookback = end;
            while (lookback > start && !std::isspace(text[lookback]) &&
                   text[lookback] != '.' && text[lookback] != ',') {
                lookback--;
            }
            if (lookback > start + (max_chars / 2)) {
                end = lookback + 1;
            }
        }

        DocumentChunk c;
        c.index = index++;
        c.text = text.substr(start, end - start);
        chunks.push_back(std::move(c));

        // Adjust next start based on snapped end to maintain overlap
        if (end < length) {
            start = end - max_chars; // Realign loop jump
        }
    }

    LOG_INFO("Document chunking: split {} chars into {} chunks", length, chunks.size());
    return chunks;
}

} // namespace vision
