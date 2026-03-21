/**
 * @file screen_observer.cpp
 * @brief "Lazy Observer" implementation — DXGI + pHash + OCR-on-delta
 *
 * DXGI Desktop Duplication gives us GPU-memory screen frames (~0.1ms).
 * We compute a 64-bit perceptual hash (~0.05ms) and only run OCR
 * when the hash changes by >5 bits. This keeps idle CPU at ~0.1%.
 *
 * DXGI Quirks handled:
 *   - DXGI_ERROR_ACCESS_LOST: screen lock, UAC, monitor sleep
 *   - Exponential backoff: 1s → 2s → 4s → 8s → 16s (capped)
 *   - Auto-reinit when desktop session returns
 */

#include "screen_observer.h"
#include <algorithm>
#include <cstring>
#include <cmath>

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define LOG_WARN(...)  spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...)  (void)0
#define LOG_WARN(...)  (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace vision {

// ═══════════════════ Constructor / Destructor ════════════════════════

ScreenObserver::ScreenObserver() {
    ring_.fill(ScreenSnapshot{});
}

ScreenObserver::~ScreenObserver() {
    stop();
    releaseDXGI();
}

// ═══════════════════ DXGI Initialization ═════════════════════════════

bool ScreenObserver::initDXGI() {
    // Create D3D11 device
    D3D_FEATURE_LEVEL feature_level;
    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // No software rasterizer
        0,                          // No flags
        nullptr, 0,                 // Default feature levels
        D3D11_SDK_VERSION,
        device_.ReleaseAndGetAddressOf(),
        &feature_level,
        context_.ReleaseAndGetAddressOf()
    );
    if (FAILED(hr)) {
        LOG_ERROR("ScreenObserver: D3D11CreateDevice failed: 0x{:08X}", (unsigned)hr);
        return false;
    }

    // Get DXGI device → adapter → output (primary monitor)
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = device_->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));
    if (FAILED(hr)) { releaseDXGI(); return false; }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) { releaseDXGI(); return false; }

    // Primary monitor = Output 0
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, output.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("ScreenObserver: No display output found");
        releaseDXGI();
        return false;
    }

    // Get IDXGIOutput1 for duplication API
    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output->QueryInterface(IID_PPV_ARGS(output1.GetAddressOf()));
    if (FAILED(hr)) { releaseDXGI(); return false; }

    // Create desktop duplication
    hr = output1->DuplicateOutput(device_.Get(), duplication_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("ScreenObserver: DuplicateOutput failed: 0x{:08X}", (unsigned)hr);
        releaseDXGI();
        return false;
    }

    LOG_INFO("ScreenObserver: DXGI Desktop Duplication initialized (primary monitor)");
    return true;
}

void ScreenObserver::releaseDXGI() {
    // ComPtr::Reset() calls Release() automatically — no manual cleanup!
    staging_tex_.Reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
}

bool ScreenObserver::reinitDXGI() {
    releaseDXGI();
    return initDXGI();
}

// ═══════════════════ Frame Capture ═══════════════════════════════════

ScreenObserver::CapturedFrame ScreenObserver::captureFrame() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif

    CapturedFrame frame;
    if (!duplication_) return frame;

    DXGI_OUTDUPL_FRAME_INFO frame_info;
    Microsoft::WRL::ComPtr<IDXGIResource> resource;

    // Try to acquire (timeout = 100ms — don't block too long)
    HRESULT hr = duplication_->AcquireNextFrame(100, &frame_info, resource.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame — screen hasn't changed (this is fine)
        return frame;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
        // Screen lock / UAC / monitor sleep — reset duplication so backoff kicks in
        LOG_WARN("ScreenObserver: DXGI access lost (screen lock/UAC?)");
        duplication_.Reset();  // BUG 10 FIX: Must nullify so workerLoop enters backoff
        return frame;
    }

    if (FAILED(hr)) {
        LOG_ERROR("ScreenObserver: AcquireNextFrame failed: 0x{:08X}", (unsigned)hr);
        return frame;
    }

    // Get the texture from the resource
    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_tex;
    hr = resource->QueryInterface(IID_PPV_ARGS(desktop_tex.GetAddressOf()));
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return frame;
    }

    // Get dimensions
    D3D11_TEXTURE2D_DESC desc;
    desktop_tex->GetDesc(&desc);
    frame.width = desc.Width;
    frame.height = desc.Height;

    // Create staging texture (CPU-readable) — recreate on resolution change
    bool need_staging = !staging_tex_;
    if (staging_tex_) {
        D3D11_TEXTURE2D_DESC existing_desc;
        staging_tex_->GetDesc(&existing_desc);
        if (existing_desc.Width != desc.Width || existing_desc.Height != desc.Height) {
            staging_tex_.Reset();  // BUG 5 FIX: Resolution changed, recreate
            need_staging = true;
        }
    }
    if (need_staging) {
        D3D11_TEXTURE2D_DESC staging_desc = desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;

        hr = device_->CreateTexture2D(&staging_desc, nullptr, staging_tex_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            duplication_->ReleaseFrame();
            return frame;
        }
    }

    // Copy GPU texture → staging texture
    context_->CopyResource(staging_tex_.Get(), desktop_tex.Get());

    // Map staging texture → CPU memory
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return frame;
    }

    // Copy pixel data (BGRA format)
    size_t row_bytes = frame.width * 4;
    frame.pixels.resize(frame.width * frame.height * 4);
    for (int y = 0; y < frame.height; y++) {
        memcpy(frame.pixels.data() + y * row_bytes,
               (uint8_t*)mapped.pData + y * mapped.RowPitch,
               row_bytes);
    }

    context_->Unmap(staging_tex_.Get(), 0);
    duplication_->ReleaseFrame();

    frame.valid = true;
    return frame;
}

// ═══════════════════ Perceptual Hash ═════════════════════════════════

uint64_t ScreenObserver::computePHash(const uint8_t* bgra, int width, int height) {
    if (width < 32 || height < 32) return 0;

    // Step 1: Downscale to 32x32 grayscale (area averaging)
    float gray[32][32] = {};
    int block_w = width / 32;
    int block_h = height / 32;

    for (int by = 0; by < 32; by++) {
        for (int bx = 0; bx < 32; bx++) {
            float sum = 0;
            int count = 0;
            int y_start = by * block_h;
            int x_start = bx * block_w;
            // Target roughly ~16 samples per block to stay fast
            int step_y = std::max(1, block_h / 4);
            int step_x = std::max(1, block_w / 4);
            for (int y = y_start; y < y_start + block_h; y += step_y) {
                for (int x = x_start; x < x_start + block_w; x += step_x) {
                    int idx = (y * width + x) * 4;
                    // BGRA → grayscale
                    sum += bgra[idx] * 0.114f + bgra[idx+1] * 0.587f + bgra[idx+2] * 0.299f;
                    count++;
                }
            }
            gray[by][bx] = (count > 0) ? sum / count : 0.0f;
        }
    }

    // Step 2: Compute top-left 8x8 DCT coefficients (low frequencies)
    // Precompute cosine table for massive speedup
    static float cos_table[32][8];
    static bool table_init = false;
    if (!table_init) {
        for (int i = 0; i < 32; i++) {
            for (int j = 0; j < 8; j++) {
                cos_table[i][j] = std::cos((2.0f * i + 1.0f) * j * 3.141592653589793f / 64.0f);
            }
        }
        table_init = true;
    }

    float dct[8][8];
    float total_dct = 0.0f;
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            float sum = 0.0f;
            for (int y = 0; y < 32; y++) {
                for (int x = 0; x < 32; x++) {
                    sum += gray[y][x] * cos_table[x][u] * cos_table[y][v];
                }
            }
            float cu = (u == 0) ? 0.70710678f : 1.0f;
            float cv = (v == 0) ? 0.70710678f : 1.0f;
            dct[v][u] = 0.25f * cu * cv * sum;
            
            // Accumulate mean (skip DC component at [0][0])
            if (u != 0 || v != 0) {
                total_dct += dct[v][u];
            }
        }
    }

    // Step 3: Compute mean of the 63 AC coefficients
    float mean = total_dct / 63.0f;

    // Step 4: Generate 64-bit hash (each bit = 1 if coef > mean)
    uint64_t hash = 0;
    int bit_index = 0;
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            // Compare against mean (even for DC, though DC is usually > mean)
            if (dct[v][u] > mean) {
                hash |= (1ULL << bit_index);
            }
            bit_index++;
        }
    }

    return hash;
}

int ScreenObserver::hammingDistance(uint64_t a, uint64_t b) {
    // __popcnt64 = count differing bits in XOR result (1 CPU cycle)
#ifdef _MSC_VER
    return (int)__popcnt64(a ^ b);
#else
    return __builtin_popcountll(a ^ b);
#endif
}

// ═══════════════════ Ring Buffer ═════════════════════════════════════

void ScreenObserver::pushSnapshot(ScreenSnapshot snapshot) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    ring_[ring_head_] = std::move(snapshot);
    ring_head_ = (ring_head_ + 1) % kRingSize;
    if (ring_count_ < kRingSize) ring_count_++;
}

ScreenSnapshot ScreenObserver::getLatest() const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (ring_count_ == 0) return {};
    int idx = (ring_head_ - 1 + kRingSize) % kRingSize;
    return ring_[idx];  // Returns a copy (thread-safe)
}

ScreenSnapshot ScreenObserver::getSnapshot(int frames_ago) const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (frames_ago < 0 || frames_ago >= ring_count_) return {};
    int idx = (ring_head_ - 1 - frames_ago + kRingSize * 2) % kRingSize;
    return ring_[idx];
}

std::string ScreenObserver::getContextText(int max_snapshots) const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (ring_count_ == 0) return "[No screen data available]";

    std::string result;
    int count = std::min(max_snapshots, ring_count_);
    for (int i = 0; i < count; i++) {
        int idx = (ring_head_ - 1 - i + kRingSize * 2) % kRingSize;
        const auto& snap = ring_[idx];
        if (!snap.valid || snap.text.empty()) continue;

        auto age = std::chrono::steady_clock::now() - snap.timestamp;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(age).count();

        if (i == 0) {
            result += "[Screen now (" + std::to_string(secs) + "s ago)]:\n";
        } else {
            result += "\n[Screen " + std::to_string(secs) + "s ago]:\n";
        }
        // Truncate to 500 chars per snapshot to avoid flooding LLM context
        if (snap.text.size() > 500) {
            result += snap.text.substr(0, 500) + "...";
        } else {
            result += snap.text;
        }
    }
    return result;
}

int ScreenObserver::snapshotCount() const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    return ring_count_;
}

// ═══════════════════ Background Worker ═══════════════════════════════

void ScreenObserver::start(OCRCallback ocr_fn, int interval_ms) {
    if (running_.load()) return;  // Already running

    ocr_fn_ = std::move(ocr_fn);
    interval_ms_ = interval_ms;
    stop_requested_ = false;
    running_ = true;  // BUG 4 FIX: Set BEFORE thread launch to prevent race

    worker_ = std::thread([this]() { workerLoop(); });
    LOG_INFO("ScreenObserver: Background thread started (interval={}ms)", interval_ms_);
}

void ScreenObserver::stop() {
    stop_requested_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
    LOG_INFO("ScreenObserver: Background thread stopped");
}

void ScreenObserver::workerLoop() {
    // Initialize DXGI on the worker thread (COM/D3D must be on same thread)
    if (!initDXGI()) {
        LOG_ERROR("ScreenObserver: Failed to init DXGI — observer disabled");
        return;
    }

    uint64_t last_hash = 0;
    auto last_ocr_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    int backoff_ms = 0;  // Exponential backoff for ACCESS_LOST

    while (!stop_requested_.load()) {
        // ── Backoff handling (DXGI blind spot trap) ──────────────
        if (backoff_ms > 0) {
            // Exponential backoff: sleep, then try to reinitialize
            LOG_WARN("ScreenObserver: Backing off {}ms (DXGI recovery)", backoff_ms);

            // Sleep in small chunks so we can respond to stop_requested
            int slept = 0;
            while (slept < backoff_ms && !stop_requested_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                slept += 100;
            }
            if (stop_requested_.load()) break;

            // Try to reinitialize DXGI
            if (reinitDXGI()) {
                LOG_INFO("ScreenObserver: DXGI reinitialized after backoff");
                backoff_ms = 0;  // Reset backoff on success
            } else {
                // Double the backoff (capped at 16s)
                backoff_ms = std::min(backoff_ms * 2, 16000);
                continue;
            }
        }

        // ── Capture frame ───────────────────────────────────────
        auto frame = captureFrame();

        // Check for DXGI errors that need reinit
        if (!frame.valid && !duplication_) {
            // DXGI lost — enter backoff
            backoff_ms = (backoff_ms == 0) ? 1000 : std::min(backoff_ms * 2, 16000);
            continue;
        }

        if (!frame.valid) {
            // Just a timeout (no new frame) — sleep and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
            continue;
        }

        // ── Compute pHash ───────────────────────────────────────
        uint64_t hash = computePHash(frame.pixels.data(), frame.width, frame.height);
        int distance = hammingDistance(hash, last_hash);

        // ── Check if screen changed enough for OCR ──────────────
        auto now = std::chrono::steady_clock::now();
        auto since_last_ocr = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_ocr_time).count();

        if (distance > kHashChangeThreshold && since_last_ocr >= kMinOCRIntervalMs) {
            // Screen changed significantly — run OCR!
            last_hash = hash;

            if (ocr_fn_) {
                std::string text = ocr_fn_(frame.pixels.data(), frame.width, frame.height);

                if (!text.empty()) {
                    ScreenSnapshot snap;
                    snap.text = std::move(text);
                    snap.phash = hash;
                    snap.timestamp = now;
                    snap.width = frame.width;
                    snap.height = frame.height;
                    snap.valid = true;

                    pushSnapshot(std::move(snap));
                    last_ocr_time = now;
                }
            }
        } else if (last_hash == 0) {
            // First frame — always hash it
            last_hash = hash;
        }

        // ── Sleep until next poll ───────────────────────────────
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }

    releaseDXGI();
}

// ═══════════════════ JPEG Frame Capture ══════════════════════════════

std::vector<uint8_t> ScreenObserver::getCurrentFrameJpeg(int quality) const {
    std::vector<uint8_t> jpeg_data;

    // We need to do a one-shot DXGI capture. Since the observer may or may not
    // be running, we use a simpler GDI fallback for maximum reliability.
    // GDI BitBlt is ~2ms for a full screen — acceptable for one-shot.

    HDC hScreen = GetDC(nullptr);
    if (!hScreen) return jpeg_data;

    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);

    HDC hMemDC = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, cx, cy);
    auto hOld = SelectObject(hMemDC, hBmp);
    BitBlt(hMemDC, 0, 0, cx, cy, hScreen, 0, 0, SRCCOPY);
    SelectObject(hMemDC, hOld);
    ReleaseDC(nullptr, hScreen);

    // Encode HBITMAP → JPEG via GDI+
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    if (Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr) == Gdiplus::Ok) {
        Gdiplus::Bitmap bmp(hBmp, nullptr);

        // Find JPEG encoder CLSID
        UINT num = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        if (size > 0) {
            std::vector<uint8_t> buf(size);
            auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
            Gdiplus::GetImageEncoders(num, size, encoders);

            CLSID jpegClsid{};
            for (UINT i = 0; i < num; i++) {
                if (wcscmp(encoders[i].MimeType, L"image/jpeg") == 0) {
                    jpegClsid = encoders[i].Clsid;
                    break;
                }
            }

            // Encode to IStream
            IStream* pStream = nullptr;
            if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream))) {
                Gdiplus::EncoderParameters params;
                params.Count = 1;
                params.Parameter[0].Guid = Gdiplus::EncoderQuality;
                params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
                params.Parameter[0].NumberOfValues = 1;
                ULONG q = static_cast<ULONG>(quality);
                params.Parameter[0].Value = &q;

                if (bmp.Save(pStream, &jpegClsid, &params) == Gdiplus::Ok) {
                    STATSTG stat{};
                    pStream->Stat(&stat, STATFLAG_NONAME);
                    ULONG total = static_cast<ULONG>(stat.cbSize.QuadPart);
                    jpeg_data.resize(total);

                    LARGE_INTEGER li{};
                    pStream->Seek(li, STREAM_SEEK_SET, nullptr);
                    ULONG bytesRead = 0;
                    pStream->Read(jpeg_data.data(), total, &bytesRead);
                    jpeg_data.resize(bytesRead);
                }
                pStream->Release();
            }
        }
        Gdiplus::GdiplusShutdown(gdipToken);
    }

    DeleteObject(hBmp);
    DeleteDC(hMemDC);

    LOG_INFO("ScreenObserver::getCurrentFrameJpeg — captured {}x{} -> {} bytes JPEG", cx, cy, jpeg_data.size());
    return jpeg_data;
}

} // namespace vision
