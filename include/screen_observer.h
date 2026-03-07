#pragma once
/**
 * @file screen_observer.h
 * @brief "Lazy Observer" — Screen Awareness via DXGI + pHash + OCR
 *
 * Architecture:
 *   Layer 1: DXGI Desktop Duplication (every 500ms) — GPU zero-copy
 *   Layer 2: Perceptual Hash (pHash) — 64-bit, ~0.05ms
 *   Layer 3: Tesseract OCR (only on >5% change) — reuses ActionExecutor singleton
 *   Layer 4: Ring Buffer (last 10 snapshots) — std::array, zero heap alloc
 *
 * Handles DXGI_ERROR_ACCESS_LOST with exponential backoff.
 * Single-thread background worker. Primary monitor only.
 */

#include <string>
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <functional>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>  // ComPtr — RAII for COM objects

namespace vision {

// ═══════════════════ Snapshot Data ═══════════════════════════════════

struct ScreenSnapshot {
    std::string text;                               // OCR-extracted text
    uint64_t phash = 0;                             // Perceptual hash (64-bit)
    std::chrono::steady_clock::time_point timestamp; // When captured
    int width = 0;
    int height = 0;
    bool valid = false;
};

// ═══════════════════ Screen Observer ═════════════════════════════════

class ScreenObserver {
public:
    /// OCR callback type: receives BGRA pixel data + dimensions, returns text
    using OCRCallback = std::function<std::string(const uint8_t* bgra, int w, int h)>;

    ScreenObserver();
    ~ScreenObserver();

    // Non-copyable, non-movable (owns thread + COM resources)
    ScreenObserver(const ScreenObserver&) = delete;
    ScreenObserver& operator=(const ScreenObserver&) = delete;

    /// Start background observation thread
    /// @param ocr_fn  Callback to perform OCR on pixel data
    /// @param interval_ms  Polling interval (default 500ms)
    void start(OCRCallback ocr_fn, int interval_ms = 500);

    /// Stop background thread (blocks until joined)
    void stop();

    /// Is the observer currently running?
    bool isRunning() const { return running_.load(); }

    // ── Query API ──────────────────────────────────────────────────

    /// Get the latest snapshot (thread-safe copy)
    ScreenSnapshot getLatest() const;

    /// Get snapshot from N frames ago (0 = latest, max 9)
    ScreenSnapshot getSnapshot(int frames_ago) const;

    /// Get all valid snapshots as a combined text context
    std::string getContextText(int max_snapshots = 3) const;

    /// Get count of valid snapshots in ring buffer
    int snapshotCount() const;

private:
    // ── DXGI resources (ComPtr = RAII, auto-Release on destruction) ──
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_tex_;

    bool initDXGI();
    void releaseDXGI();
    bool reinitDXGI();  // For ACCESS_LOST recovery

    // ── Capture ────────────────────────────────────────────────────
    struct CapturedFrame {
        std::vector<uint8_t> pixels;  // BGRA
        int width = 0;
        int height = 0;
        bool valid = false;
    };

    CapturedFrame captureFrame();

    // ── Perceptual Hash ────────────────────────────────────────────
    /// Compute 64-bit perceptual hash from BGRA image
    static uint64_t computePHash(const uint8_t* bgra, int width, int height);

    /// Hamming distance between two hashes (number of differing bits)
    static int hammingDistance(uint64_t a, uint64_t b);

    // ── Ring Buffer (fixed-size, zero heap alloc after init) ──────
    static constexpr int kRingSize = 10;
    std::array<ScreenSnapshot, kRingSize> ring_;
    int ring_head_ = 0;        // Points to next write position
    int ring_count_ = 0;       // Number of valid entries
    mutable std::mutex ring_mutex_;

    void pushSnapshot(ScreenSnapshot snapshot);

    // ── Background thread ──────────────────────────────────────────
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    OCRCallback ocr_fn_;
    int interval_ms_ = 500;

    /// Min interval between OCR runs (throttle for video/games)
    static constexpr int kMinOCRIntervalMs = 2000;

    /// pHash change threshold (out of 64 bits)
    static constexpr int kHashChangeThreshold = 5;

    void workerLoop();
};

} // namespace vision
