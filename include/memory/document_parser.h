#pragma once
/**
 * @file document_parser.h
 * @brief Document parsing and semantic text chunking
 *
 * Extracts text from local files (TXT, MD) and splits them into
 * overlapping chunks suitable for LLM context windows.
 * Provides stubs for future PDF/DOCX integration.
 */

#include <string>
#include <vector>

namespace vision {

struct DocumentChunk {
    int index = 0;
    std::string text;
};

class DocumentParser {
public:
    /// Check if a file extension is supported
    static bool isSupported(const std::string& filepath);

    /// Parse a document into a single raw text string
    static std::string parseToText(const std::string& filepath);

    /// Chunk text into roughly `max_tokens` (estimated by chars) with `overlap_tokens`
    /// Uses 4 chars = 1 token as a heuristic for English text.
    static std::vector<DocumentChunk> chunkText(const std::string& text, 
                                                 int max_tokens = 512, 
                                                 int overlap_tokens = 50);

private:
    static std::string parseTxt(const std::string& filepath);
    static std::string parsePdfStub(const std::string& filepath);
};

} // namespace vision
