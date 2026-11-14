// KameraGeulis — Windows-only startup helpers.
// Early file logging (log.txt, flushed per line), crash handlers with a
// minidump + MessageBox, explicit app-local DLL preloading (defeats a
// stray onnxruntime.dll in System32), and the fatal-error reporting path.
// See docs/TROUBLESHOOTING.md.
#pragma once
#include <string>
#include <windows.h>

namespace kgcore {

struct StartupArgs {
  bool safe = false;     // --safe:     window + UI only — no camera, no ORT, no GPUPixel
  bool selftest = false; // --selftest: headless DLL+ORT+model smoke test, exit code 0/non-0
  bool capture_plain = false; // --capture-plain: source reader TANPA atribut (tingkat (c) M2f)
};

struct StartupPaths {
  std::wstring exe_dir;      // folder of the running exe (trailing backslash)
  std::wstring appdata_dir;  // %APPDATA%\KameraGeulis (created)
  std::wstring log_path;     // appdata_dir\log.txt
};

// Folder of the running module (trailing backslash), empty on failure.
std::wstring ExeDirFromModule(HINSTANCE module);

// Computes exe_dir + appdata_dir (creating appdata_dir if needed).
// On failure returns false and sets `err` (a MessageBox can then be shown
// because the log file may not exist yet).
bool ComputeStartupPaths(HINSTANCE module, StartupPaths* out, std::wstring* err);

StartupArgs ParseStartupArgs(const wchar_t* cmdline);

// True Windows version via RtlGetVersion, e.g. "Windows 10.0.19045 (build 19045)".
std::string WindowsVersionString();

// Opens the log file (append + per-run separator) and writes the startup
// header: app version, Windows version, command line, exe dir, flags.
// Must be called before any risky init.
void BeginFileLog(const StartupPaths& paths, const StartupArgs& args);

// Flush + close the log file (called on normal exit and inside crash handler).
void EndFileLog();

// Installs SetUnhandledExceptionFilter + std::set_terminate.
// On a fatal error: writes crash-<yyyymmdd-hhmmss>.dmp to appdata_dir, logs
// exception code + faulting module + dump path, and shows a MessageBoxW.
void InstallCrashHandlers(const StartupPaths& paths);

// Preloads onnxruntime.dll and gpupixel.dll from exe_dir via LoadLibraryExW
// (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS),
// logs the ACTUAL module path of each (catches System32 shadowing), and
// verifies the ORT API via OrtGetApiBase()->GetVersionString().
// Does NOT create sessions or touch GL. No MessageBox.
struct DllPreloadResult {
  bool all_ok = false;
  std::wstring detail;        // empty when all_ok; otherwise reason + missing file + remediation
  std::string ort_version;    // e.g. "1.31.1" ("" when ORT not loaded)
  std::wstring ort_module;    // actual path of loaded onnxruntime.dll
  std::wstring gpupixel_module;
};
DllPreloadResult PreloadRuntimeDlls(const StartupPaths& paths);

// Logs the error, shows a MessageBoxW (reason + log path), and exits.
// Used for fatal init failures (GL/GLFW/window/DLL) — never fails silently.
void ShowFatalAndExit(const wchar_t* context, const std::wstring& detail, int exit_code);

// Runs the headless smoke test (--selftest). No window, no GL, no MessageBox.
// Downloads nothing; loads DLLs from exe_dir and models from
// KAMERAGEULIS_MODELS_DIR (or <exe_dir>\models). Returns process exit code:
//   0 pass · 2 DLL preload · 3 ORT API · 4 model · 5 ORT session · 6 inference
// Implemented in selftest.cc.
int RunSelftest(const StartupPaths& paths);

}  // namespace kgcore
