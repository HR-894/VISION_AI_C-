<p align="center">
  <img src="assets/icon.png" alt="VISION AI Daemon Logo" width="128" />
</p>

<h1 align="center">VISION AI Daemon</h1>

<p align="center">
  <strong>The Sensory and Motor Layer for Next-Gen AI — A Headless C++ gRPC Daemon</strong>
</p>

<p align="center">
  <a href="#-download--install"><img src="https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D6?style=for-the-badge&logo=windows&logoColor=white" alt="Windows"></a>
  <a href="#-build-from-source"><img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++20"></a>
  <a href="#-architecture--tech-stack"><img src="https://img.shields.io/badge/gRPC-Framework-244c5a?style=for-the-badge&logo=grpc&logoColor=white" alt="gRPC"></a>
  <a href="#-core-features"><img src="https://img.shields.io/badge/whisper.cpp-Audio-FF6F00?style=for-the-badge" alt="whisper.cpp"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-A31F34?style=for-the-badge" alt="License"></a>
</p>

---

## 🎬 What is the VISION AI Daemon?

The **VISION AI Daemon** is a native, highly optimized **Windows C++ daemon** designed to act as the raw hardware interaction layer for external AI agents. It exposes the operating system's sensory (audio, screen) and motor (keyboard, mouse, windows) functions over a **strict gRPC protocol**.

By design, this daemon contains **zero cognitive logic, zero LLM integration, and zero UI**. It exists purely to convert raw OS signals into structured data, and structured commands into raw OS actions. Your external Python/Node.js/Go AI orchestrators can connect to this daemon to take control of the PC.

---

## ✨ Core Capabilities

### 🎤 Voice Input Pipeline (`AudioCapture` + `WhisperEngine`)
A lock-free, highly concurrent audio pipeline for real-time speech-to-text.
- **Lock-Free Audio Ring Buffer:** Single-Producer/Single-Consumer (SPSC) queue captures mic input without blocking the PortAudio real-time hardware thread.
- **Atomic VAD (Voice Activity Detection):** Adaptive noise floor tracking built on atomics, ensuring thread-safe gating of audio streams.
- **Asynchronous whisper.cpp Integration:** Emits streaming partial and final transcriptions without freezing the worker threads.

### 🖥️ OS-Level Automation (`ui_automation` + `window_manager`)
- **UI Automation:** Microsoft UIA (COM) integration for reading accessibility trees, clicking buttons, and semantic form interactions.
- **Screen Observer:** DXGI Desktop Duplication API + perceptual hashing (pHash) to detect screen deltas and visual state.
- **Smart Input Injection:** Win32 `SendInput` with caret tracking to inject keystrokes reliably into target processes.
- **Window Control:** Full Win32 API access to manipulate, focus, and query OS windows.

### 🩺 System Diagnostics (`doctor`)
Basic system health monitoring exposed via RPC:
- RAM usage & capacity
- CPU logical core count
- Filesystem available space

---

## 🏗️ Architecture

```
VISION AI Daemon (Headless gRPC)
├── Language          C++20 (MSVC /std:c++20)
├── Communication     gRPC + Protobuf
├── Speech-to-Text    whisper.cpp (ggml models)
├── Audio Capture     PortAudio 19
├── Audio Pipeline    Lock-Free SPSC Ring Buffer + VAD
├── UI Automation     Microsoft UIA (COM)
├── Screen Capture    DXGI Desktop Duplication API
├── Input Simulation  Win32 SendInput + GUI Thread Caret
├── Logging           spdlog (rotating file + console)
└── Build System      CMake 3.20+
```

### The gRPC Contract (`vision_daemon.proto`)
The system communicates strictly through `vision_daemon.proto`. Supported services include:
- `HealthCheck()`: Returns RAM/CPU status
- `StreamVoice()`: Bidirectional streaming for real-time transcription
- `ExecuteOSCommand()`: Injection for Win32/UIA actions

---

## 🛠️ Build from Source

### Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| **CMake** | ≥ 3.20 | Build system |
| **MSVC** | VS 2022+ | C++20 compiler |
| **gRPC & Protobuf** | 1.5x+ | For RPC generation |
| **OpenCV** | 4.x | *Optional* — OCR & template matching |
| **PortAudio** | 19.x | Microphone capture |

### Clone & Build

```bash
# Clone with submodules (whisper.cpp, spdlog)
git clone --recursive https://github.com/HR-894/VISION_AI_C-.git
cd VISION_AI_C-

# Configure (Generates gRPC C++ bindings automatically)
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..

# Build
cmake --build . --config Release --parallel

# Run Daemon
./bin/Release/vision_daemon.exe
```

---

## 🤝 Contributing

Contributions welcome! Bug fixes, features, docs — all appreciated.

1. **Fork** the repository
2. **Create** a feature branch: `git checkout -b feat/amazing-feature`
3. **Commit** your changes: `git commit -m "feat: add amazing feature"`
4. **Push** to the branch: `git push origin feat/amazing-feature`
5. **Open** a Pull Request

**Code style:** C++20, `snake_case` for files, `PascalCase` for classes, compile clean with `/W4`. No UI or Qt dependencies allowed.

---

## 📜 License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.
