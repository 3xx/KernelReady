<p align="center">
  <img width="800" height="500" alt="KernelReady Banner" src="https://github.com/user-attachments/assets/ece8bd5f-d92c-4f85-90ab-36ac61cd80dd" />
</p>

<h1 align="center">KernelReady</h1>

<p align="center">
  <b>One command to prepare your Windows Kernel/Driver development environment.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows-blue" />
  <img src="https://img.shields.io/badge/toolchain-MSVC-orange" />
  <img src="https://img.shields.io/badge/license-MIT-green" />
  <img src="https://img.shields.io/badge/status-stable-success" />
</p>

---

## ⚠️ Prerequisites (Important)

Before running **KernelReady**, you must install the Microsoft Visual C++ Redistributables:

- x86: https://aka.ms/vc14/vc_redist.x86.exe  
- x64: https://aka.ms/vc14/vc_redist.x64.exe  

❗ KernelReady will not run correctly without these dependencies installed.

---

## 🚀 Overview

KernelReady is a smart bootstrapper for Windows Kernel/Driver development.  
It installs, verifies, and repairs Visual Studio, SDK, and WDK to ensure a consistent and ready-to-use development environment.

---

## ✨ Features

- ⚙️ Automated setup of Visual Studio 2022 Professional  
- 📦 Installs and verifies Windows SDK & WDK  
- 🧠 Smart environment detection & compatibility planning  
- 🔧 Repair and force reinstall modes  
- 📄 Detailed logs and machine-readable summaries  
- ⚡ One-command setup  

---

## 📦 What It Sets Up

- Visual Studio 2022 (Professional)
- MSVC Toolchain
- Windows SDK (10.0.26100+)
- Windows Driver Kit (WDK)
- Kernel development toolchain

---

## ⚙️ Usage

    KernelReady.exe

### Modes

    KernelReady.exe --verify-only   # Check environment only
    KernelReady.exe --dry-run       # Simulate setup
    KernelReady.exe --repair        # Repair installation
    KernelReady.exe --force         # Force reinstall

---

## 🔍 Example

    KernelReady.exe --verify-only

Output:

    all_critical_passed: true
    Exit code: 0

---

## 📊 Logs

    %LOCALAPPDATA%\KernelReady\Logs\

Each run generates:
- Detailed log file  
- Execution summary  

---

## 🧠 Why KernelReady?

- ❌ No more manual setup errors  
- 🔁 Consistent environments across machines  
- 🧪 Reliable verification and diagnostics  
- 🏗️ Built using official Microsoft installers  

---

## ⚠️ Notes

- Requires Administrator privileges  
- Internet connection is required  
- Does not uninstall existing Visual Studio installations  

---

## 📄 License

MIT License

---

## 💡 Philosophy

> "No more 'it works on my machine' — KernelReady makes every machine ready."
