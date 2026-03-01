<p align="center">
  <img src="images/vision_ai_logo.png" alt="VISION AI Logo" width="120" />
</p>

<h1 align="center">VISION AI</h1>

<p align="center">
  <strong>The Ultimate Offline-First, Privacy-Focused Windows AI Copilot — Built in Modern C++.</strong>
</p>

<p align="center">
  <a href="#-download--install"><img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D6?style=for-the-badge&logo=windows&logoColor=white" alt="Windows"></a>
  <a href="#-build-from-source"><img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++20"></a>
  <a href="#-architecture--tech-stack"><img src="https://img.shields.io/badge/Qt-6-41CD52?style=for-the-badge&logo=qt&logoColor=white" alt="Qt6"></a>
  <a href="#-core-features"><img src="https://img.shields.io/badge/100%25-Offline-FF6F00?style=for-the-badge&logo=wifi-off&logoColor=white" alt="Offline"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-A31F34?style=for-the-badge" alt="License"></a>
</p>

<p align="center">
  <em>Zero cloud. Zero latency. Zero data leaves your machine — ever.</em>
</p>

---

## 📖 Overview

**VISION AI** is a native Windows desktop application that acts as an autonomous AI copilot for your PC. It reads your screen, understands your intent via voice or text, and executes complex, multi-step workflows — all **entirely offline**, with **zero data transmission**, and at **native C++ speed**.

Unlike cloud-dependent copilots that stream your keystrokes, screenshots, and voice to remote servers:

| | Cloud Copilots | **VISION AI** |
|---|---|---|
| **Privacy** | Your data on someone else's server | 🔒 Everything stays on your machine |
| **Latency** | Network round-trip per request | ⚡ Instant — native CPU/GPU inference |
| **Availability** | Requires internet connection | 🌐 Works on airplane mode |
| **Cost** | Monthly subscription fees | 💰 Free & open-source, forever |

Built from the ground up in **C++20** with **Qt6**, VISION AI is engineered to run beautifully on hardware as modest as **8 GB RAM** while scaling up to leverage **RTX GPUs** for maximum throughput.

---

## ✨ Core Features

### 🧠 True Offline AI Processing

VISION AI runs LLM inference and speech recognition **entirely on your hardware** — no API keys, no internet, no compromise.

- **LLM Inference** — Powered by [`llama.cpp`](https://github.com/ggerganov/llama.cpp) with GGUF quantized models. Full reasoning, planning, and code generation without a cloud in sight.
- **Instant Voice Transcription** — Integrated [`whisper.cpp`](https://github.com/ggerganov/whisper.cpp) converts speech to text in real-time, on-device. Just press `Ctrl+Win` and speak.

---

### ⚙️ Hardware-Aware Optimization

A built-in **`DeviceProfiler`** fingerprints your system at startup and dynamically tunes every performance knob:

| System Tier | RAM | GPU | Context Window | CPU Threads |
|---|---|---|---|---|
| 🟡 Low-End | 8 GB | Integrated | 2,048 tokens | Auto (conservative) |
| 🟢 Mid-Range | 16 GB | GTX 1650+ | 4,096 tokens | Auto (balanced) |
| 🔵 High-End | 32 GB+ | RTX 4050+ | 8,192 tokens | Auto (max) |

- **Dynamic Context Window Sizing** — Automatically scales the LLM context between 2K–8K tokens based on available memory.
- **Dynamic CPU Thread Allocation** — Detects physical cores, respects thermal headroom, and assigns threads proportionally.
- **GPU Auto-Detection** — Build system auto-selects the fastest backend: **CUDA** → **Vulkan** → **CPU** fallback.

---

### 🖥️ OS-Level UI Automation

VISION AI doesn't just "see" your screen — it **understands** it.

- **Microsoft UI Automation (UIA)** — Reads the live accessibility tree of any application to identify buttons, text fields, menus, and more. Clicks and interacts with elements by semantic name — no brittle coordinate hacking.
- **Tesseract OCR Fallback** — When an app lacks accessibility metadata (e.g., legacy Win32 apps, games), VISION AI falls back to **Tesseract OCR** + **OpenCV** template matching to visually locate UI elements.

---

### ⌨️ Smart Input Simulation

Typing text into other applications sounds trivial — until the OS swallows characters because the target thread isn't ready.

- **Win32 GUI Thread Caret Tracking** — Monitors the caret state of the target window's GUI thread to detect exactly when it's safe to inject the next keystroke.
- **"Smart Wait" Algorithm** — Adaptively pauses between keystrokes based on real-time thread responsiveness, ensuring **zero dropped characters** even in heavy applications.

---

### 🗄️ Semantic Vector Memory

VISION AI remembers your past tasks and learns your patterns — without a single external database.

- **Pure C++ Cosine Similarity** — Computes vector similarity directly from LLM logits to find relevant past interactions, eliminating the need for bloated vector-database dependencies (no ChromaDB, no Pinecone).
- **Behavioral Learning** — Tracks user command patterns in memory to suggest faster, more contextual actions over time.

---

### 🛡️ Military-Grade Privacy

Your data never leaves your machine, and even **local storage is hardened**:

- **DPAPI Encryption at Rest** — Agent memory files and user behavior JSONs are encrypted using [Windows Data Protection API (DPAPI)](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/), tied to your Windows user credentials.
- **Strict File Whitelisting** — A `SafetyGuard` subsystem prevents the AI from accessing, modifying, or deleting any file not on an explicit whitelist. Blocks dangerous hallucinations before they become dangerous actions.
- **No Telemetry** — Zero analytics, zero crash reports, zero network calls. `netstat` will confirm: VISION AI is silent on the wire.

---

## 📥 Download & Install

### Pre-Built Release (Recommended)

1. Go to the [**Releases**](../../releases) tab.
2. Download the latest `VISION_AI_vX.X.X_Setup.exe` or the portable `.zip`.
3. Run the installer or extract the archive.
4. Launch `VISION_AI.exe`.

### Automatic Model Downloader

On first launch, VISION AI will detect that no AI models are present and offer to download them automatically:

- **LLM Model** — A quantized GGUF model (e.g., `Phi-3-mini-Q4_K_M.gguf`) optimized for your hardware tier.
- **Whisper Model** — A compact `ggml-base.en.bin` for fast English speech recognition.

Models are stored locally in the `models/` directory next to the executable. No account required, no sign-up — just click **Download** and you're ready.

> **💡 Tip:** You can also manually place any GGUF-compatible model into the `models/` folder. VISION AI will auto-detect and use it.

---

## 🛠️ Build from Source

### Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| **CMake** | ≥ 3.20 | Build system |
| **MSVC** | VS 2022+ | C++20 compiler |
| **Qt6** | 6.x | `Widgets`, `Core` modules |
| **OpenCV** | 4.x | *Optional* — for OCR & template matching |
| **Tesseract** | 5.x | *Optional* — OCR fallback engine |
| **PortAudio** | 19.x | *Optional* — microphone capture |
| **CUDA Toolkit** | 12.x | *Optional* — NVIDIA GPU acceleration |
| **Vulkan SDK** | 1.3+ | *Optional* — AMD/Intel GPU acceleration |

### Clone & Build

```bash
# Clone with submodules (llama.cpp, whisper.cpp, etc.)
git clone --recursive https://github.com/HR-894/VISION_AI_C-.git
cd VISION_AI_C-

# Configure (GPU backend is auto-detected)
cmake -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"

# Build
cmake --build build --config Release --parallel

# Run
./build/bin/Release/VISION_AI.exe
```

### CMake Options

| Option | Default | Description |
|---|---|---|
| `VISION_ENABLE_LLM` | `ON` | Enable llama.cpp LLM inference |
| `VISION_ENABLE_WHISPER` | `ON` | Enable whisper.cpp speech-to-text |
| `VISION_ENABLE_OCR` | `ON` | Enable Tesseract OCR |
| `VISION_ENABLE_AUDIO` | `ON` | Enable PortAudio microphone capture |
| `VISION_BUILD_TESTS` | `OFF` | Build unit tests |

> **GPU Backend** is auto-detected at configure time: **CUDA** (NVIDIA) → **Vulkan** (AMD/Intel/NVIDIA) → **CPU** fallback. No manual flags needed.

---

## 🏗️ Architecture & Tech Stack

```
VISION AI v3.0.0
├── Language        C++20 (MSVC /std:c++20)
├── UI Framework    Qt 6 (Widgets, Core)
├── LLM Engine      llama.cpp (GGUF models, CUDA/Vulkan/CPU)
├── Speech-to-Text  whisper.cpp (ggml models)
├── OCR             Tesseract 5 + OpenCV 4
├── Audio Capture   PortAudio 19
├── JSON            nlohmann/json (header-only)
├── Logging         spdlog
├── UI Automation   Microsoft UI Automation (UIA) via COM
├── Input Sim       Win32 SendInput + GUI Thread Caret Tracking
├── Encryption      Windows DPAPI (CryptProtectData)
├── Vector Memory   Pure C++ Cosine Similarity (no external DB)
└── Build System    CMake 3.20+
```

### Module Map

| Module | File | Role |
|---|---|---|
| **Core App** | `vision_ai.cpp` | Main application orchestrator & Qt UI |
| **LLM Controller** | `llm_controller.cpp` | llama.cpp model loading, inference, context management |
| **ReAct Agent** | `react_agent.cpp` | Reason-Act loop for multi-step task execution |
| **Action Executor** | `action_executor.cpp` | Translates agent decisions into OS actions |
| **Whisper Engine** | `whisper_engine.cpp` | Real-time speech-to-text pipeline |
| **Audio Capture** | `audio_capture.cpp` | PortAudio microphone stream management |
| **Device Profiler** | `device_profiler.cpp` | Hardware detection & performance tuning |
| **UI Automation** | `ui_automation.cpp` | Microsoft UIA accessibility tree traversal |
| **Window Manager** | `window_manager.cpp` | Window enumeration, focus, & input injection |
| **System Commands** | `system_commands.cpp` | Volume, brightness, app launching, system control |
| **Command Router** | `command_router.cpp` | Intent classification & command dispatch |
| **Safety Guard** | `safety_guard.cpp` | File whitelisting & action validation |
| **Agent Memory** | `agent_memory.cpp` | DPAPI-encrypted semantic memory store |
| **User Behavior** | `user_behavior.cpp` | Behavioral pattern learning & prediction |
| **Config Manager** | `config_manager.cpp` | Persistent settings & runtime configuration |
| **File Manager** | `file_manager.cpp` | Safe file operations with sandboxing |
| **Context Manager** | `context_manager.cpp` | Conversation history & context window management |
| **GPU Setup Wizard** | `gpu_setup_wizard.cpp` | First-run GPU detection & optimal backend selection |
| **Template Matcher** | `smart_template_matcher.cpp` | OpenCV-based visual element matching |
| **Web Search** | `web_search.cpp` | Minimal web search integration |

---

## 🤝 Contributing

Contributions are welcome! Whether it's a bug fix, a new feature, or better documentation — we'd love your help.

1. **Fork** the repository.
2. **Create** a feature branch: `git checkout -b feat/amazing-feature`
3. **Commit** your changes: `git commit -m "feat: add amazing feature"`
4. **Push** to the branch: `git push origin feat/amazing-feature`
5. **Open** a Pull Request.

Please ensure your code:

- Compiles cleanly with `/W4` on MSVC.
- Follows the existing code style (C++20, `snake_case` for files, `PascalCase` for classes).
- Includes a clear description of what the change does and why.

---

## 📜 License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

---

<p align="center">
  <strong>VISION AI</strong> — Your PC. Your Data. Your AI.<br/>
  <sub>Built with ❤️ in Modern C++</sub>
</p>
