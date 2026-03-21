/**
 * @file test_vector_memory.cpp
 * @brief Unit tests for VectorMemory core math and data structures
 */

#include <gtest/gtest.h>
#include "memory/vector_memory.h"
#include <string>
#include <vector>
#include <cmath>

using namespace vision;

class VectorMemoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup a deterministic embedding function
        mem.setEmbeddingFn([](const std::string& text) {
            return VectorMemory::generateTrigramEmbedding(text);
        });
    }

    VectorMemory mem;
};

TEST_F(VectorMemoryTest, TrigramEmbeddingDeterministic) {
    auto embed1 = VectorMemory::generateTrigramEmbedding("hello world");
    auto embed2 = VectorMemory::generateTrigramEmbedding("hello world");
    
    ASSERT_EQ(embed1.size(), 768);
    ASSERT_EQ(embed2.size(), 768);
    
    for (size_t i = 0; i < 768; i++) {
        EXPECT_FLOAT_EQ(embed1[i], embed2[i]);
    }
}

TEST_F(VectorMemoryTest, StoreAndSearchExactMatch) {
    mem.store("the quick brown fox", "test_context_1", {"animal"});
    mem.store("jumps over the lazy dog", "test_context_2", {"animal"});
    
    auto results = mem.search("the quick brown fox", 1);
    
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].entry.context, "test_context_1");
    EXPECT_GT(results[0].similarity, 0.95f); // Should be exactly 1.0 ideally
}

TEST_F(VectorMemoryTest, DPAPIPersistenceCheck) {
    mem.store("persistence test data", "ctx", {"test"});
    
    // Save to temp file
    std::string test_file = "test_vmem.bin";
    EXPECT_TRUE(mem.save(test_file));
    
    // Load into new memory
    VectorMemory loaded_mem;
    EXPECT_TRUE(loaded_mem.load(test_file));
    
    EXPECT_EQ(loaded_mem.size(), 1);
    
    // Test that embedding search still works
    loaded_mem.setEmbeddingFn([](const std::string& text) {
        return VectorMemory::generateTrigramEmbedding(text);
    });
    
    auto results = loaded_mem.search("persistence test data", 1);
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].entry.context, "ctx");
    EXPECT_EQ(results[0].entry.tags[0], "test");
    
    std::remove(test_file.c_str());
}

TEST_F(VectorMemoryTest, MaximumCapacityEviction) {
    // Fill beyond capacity
    for (int i = 0; i < 10005; i++) {
        mem.store("Dummy data " + std::to_string(i));
    }
    
    // Size should be capped at kMaxMemoryEntries (10000)
    EXPECT_EQ(mem.size(), mem.capacity());
}
