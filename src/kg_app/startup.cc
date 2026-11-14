#include "startup.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include <dbghelp.h>
#include <shellapi.h>
#include <winternl.h>

#include "kgcore/log.h"
#include "onnxruntime_c_api.h"

namespace kgcore {
namespace {

std::string Utf8FromWide(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                    nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), n,
                      nullptr, nullptr);
  return out;
}

// ---- crash handler state (set once; read from the unhandled filter) ----
wchar_t g_appdata_dir[MAX_PATH]{};
wchar_t g_log_path[MAX_PATH]{};

bool TimestampedDumpPath(wchar_t* out, size_t out_cch) {
  std::time_t now = std::time(nullptr);
  std::tm tm_now{};
  localtime_s(&tm_now, &now);
  wchar_t name[MAX_PATH]{};
  swprintf_s(name, MAX_PATH, L"\\crash-%04d%02d%02d-%02d%02d%02d.dmp", tm_now.tm_year + 1900,
             tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
  const size_t n = wcslen(g_appdata_dir) + wcslen(name);
  if (n + 1 > out_cch) return false;
  wcscpy_s(out, out_cch, g_appdata_dir);
  wcscat_s(out, out_cch, name);
  return true;
}

std::wstring FaultingModuleName(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord) return L"(tidak diketahui)";
  HMODULE hmod = nullptr;
  const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
  if (!GetModuleHandleExW(flags,
                          reinterpret_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress),
                          &hmod) ||
      !hmod) {
    return L"(tidak diketahui)";
  }
  wchar_t path[MAX_PATH]{};
  if (GetModuleFileNameW(hmod, path, MAX_PATH) == 0) return L"(tidak diketahui)";
  std::wstring full(path);
  const size_t slash = full.find_last_of(L"\\/");
  return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

bool WriteMiniDump(const wchar_t* dump_path, EXCEPTION_POINTERS* ep) {
  HANDLE hfile = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hfile == INVALID_HANDLE_VALUE) return false;
  const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
      MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
  bool ok = false;
  if (ep) {
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hfile, type, &mei,
                           nullptr, nullptr) == TRUE;
  } else {
    ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hfile, type, nullptr,
                           nullptr, nullptr) == TRUE;
  }
  CloseHandle(hfile);
  return ok;
}

void ReportFatal(const wchar_t* what, DWORD exception_code, EXCEPTION_POINTERS* ep) {
  wchar_t dmp[MAX_PATH]{};
  const bool dmp_ok = TimestampedDumpPath(dmp, MAX_PATH) && WriteMiniDump(dmp, ep);
  const std::wstring module = FaultingModuleName(ep);
  kgcore::Log("crash", "FATAL %ls (exception 0x%08lX) — faulting module: %ls", what,
              exception_code, module.c_str());
  kgcore::Log("crash", "dump: %ls", dmp_ok ? dmp : L"(gagal membuat dump)");
  kgcore::Log("crash", "log:  %ls", g_log_path);

  wchar_t msg[2048]{};
  swprintf_s(msg, 2048,
             L"KameraGeulis berhenti karena error fatal.\n\n"
             L"Detail: %ls\n"
             L"Exception: 0x%08lX\n"
             L"Faulting module: %ls\n"
             L"Log:  %ls\n"
             L"Dump: %ls%ls\n\n"
             L"Kirim log.txt dan file .dmp (dan screenshot bila ada) ke developer.\n"
             L"Lihat docs/TROUBLESHOOTING.md di repo untuk cara mengumpulkan data ini.",
             what, exception_code, module.c_str(), g_log_path, dmp,
             dmp_ok ? L"" : L" (GAGAL DIBUAT — lihat folder %%APPDATA%%\\KameraGeulis)");
  MessageBoxW(nullptr, msg, L"KameraGeulis — Crash",
              MB_ICONERROR | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
  kgcore::CloseLogFile();
}

LONG WINAPI TopLevelUnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
  ReportFatal(L"unhandled exception", code, ep);
  return EXCEPTION_EXECUTE_HANDLER;
}

void TopLevelTerminate() {
  ReportFatal(L"C++ exception tidak tertangkap (std::terminate)", 0, nullptr);
  ExitProcess(3);
}

// ---- DLL preloading ----
HMODULE LoadFromExeDir(const std::wstring& exe_dir, const wchar_t* name,
                       std::wstring* actual_path, DWORD* error_out) {
  const std::wstring full = exe_dir + name;
  // Full path => this exact module loads (never a System32 copy with the
  // same file name). LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR makes ITS dependencies
  // (e.g. vcruntime140.dll) resolve next to it first; DEFAULT_DIRS keeps
  // System32 reachable only as a last resort for unrelated deps.
  HMODULE h = LoadLibraryExW(full.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!h) {
    *error_out = GetLastError();
    return nullptr;
  }
  wchar_t path[MAX_PATH]{};
  if (GetModuleFileNameW(h, path, MAX_PATH) != 0) *actual_path = path;
  return h;
}

std::wstring MissingDllDetail(const std::wstring& exe_dir, const wchar_t* name, DWORD err) {
  wchar_t buf[1024]{};
  swprintf_s(buf, 1024,
             L"DLL tidak ditemukan/gagal dimuat: %ls (kode error %lu)\n"
             L"Folder aplikasi: %ls\n\n"
             L"Penyebab umum: (1) folder hasil ekstrak tidak lengkap — unzip ulang artifact ke "
             L"folder BARU (jangan salin exe saja); (2) antivirus/SmartScreen memblokir DLL; "
             L"(3) file rusak saat unduhan — unduh ulang artifact.\n"
             L"Kode 126 = dependency dari DLL itu yang hilang; 193 = bukan DLL 64-bit yang valid.",
             name, err, exe_dir.c_str());
  return buf;
}

}  // namespace

std::wstring ExeDirFromModule(HINSTANCE module) {
  wchar_t buf[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(module, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::wstring path(buf);
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  return path.substr(0, slash + 1);
}

bool ComputeStartupPaths(HINSTANCE module, StartupPaths* out, std::wstring* err) {
  out->exe_dir = ExeDirFromModule(module);
  if (out->exe_dir.empty()) {
    *err = L"GetModuleFileName gagal — tidak bisa menentukan folder aplikasi.";
    return false;
  }
  wchar_t appdata[MAX_PATH]{};
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    *err = L"Variabel lingkungan APPDATA tidak tersedia — tidak bisa menulis log/pengaturan.";
    return false;
  }
  out->appdata_dir = std::wstring(appdata) + L"\\KameraGeulis";
  if (CreateDirectoryW(out->appdata_dir.c_str(), nullptr) == 0 &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    *err = L"Gagal membuat folder " + out->appdata_dir + L" (kode " +
           std::to_wstring(GetLastError()) + L").";
    return false;
  }
  out->log_path = out->appdata_dir + L"\\log.txt";
  return true;
}

StartupArgs ParseStartupArgs(const wchar_t* cmdline) {
  StartupArgs args;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(cmdline, &argc);
  if (!argv) return args;
  for (int i = 1; i < argc; ++i) {
    if (wcscmp(argv[i], L"--safe") == 0) args.safe = true;
    if (wcscmp(argv[i], L"--selftest") == 0) args.selftest = true;
    if (wcscmp(argv[i], L"--capture-plain") == 0) args.capture_plain = true;
  }
  LocalFree(argv);
  return args;
}

std::string WindowsVersionString() {
  using RtlGetVersionFn = NTSTATUS(WINAPI*)(PRTL_OSVERSIONINFOW);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto* fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
                   : nullptr;
  RTL_OSVERSIONINFOW vi{};
  vi.dwOSVersionInfoSize = sizeof(vi);
  if (fn && fn(&vi) == 0) {
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), "Windows %lu.%lu.%lu (build %lu)", vi.dwMajorVersion,
                  vi.dwMinorVersion, vi.dwBuildNumber, vi.dwBuildNumber);
    return buf;
  }
  return "(versi Windows tidak diketahui)";
}

void BeginFileLog(const StartupPaths& paths, const StartupArgs& args) {
  kgcore::SetLogFile(Utf8FromWide(paths.log_path));
  kgcore::Log("startup", "==================================================");
  kgcore::Log("startup", "KameraGeulis 0.2.0 — sesi baru");
  kgcore::Log("startup", "windows: %s", WindowsVersionString().c_str());
  kgcore::Log("startup", "cmdline: %ls", GetCommandLineW());
  kgcore::Log("startup", "exe_dir: %ls", paths.exe_dir.c_str());
  kgcore::Log("startup", "log: %ls", paths.log_path.c_str());
  kgcore::Log("startup", "flags: --safe=%d --selftest=%d --capture-plain=%d",
              args.safe ? 1 : 0, args.selftest ? 1 : 0, args.capture_plain ? 1 : 0);
}

void EndFileLog() { kgcore::CloseLogFile(); }

void InstallCrashHandlers(const StartupPaths& paths) {
  wcsncpy_s(g_appdata_dir, MAX_PATH, paths.appdata_dir.c_str(), _TRUNCATE);
  wcsncpy_s(g_log_path, MAX_PATH, paths.log_path.c_str(), _TRUNCATE);
  SetUnhandledExceptionFilter(TopLevelUnhandledExceptionFilter);
  std::set_terminate(TopLevelTerminate);
  kgcore::Log("startup", "crash handler terpasang (dump ke %ls\\crash-*.dmp)", g_appdata_dir);
}

DllPreloadResult PreloadRuntimeDlls(const StartupPaths& paths) {
  DllPreloadResult result;
  const std::wstring dir = paths.exe_dir;

  // Route the default DLL search through the app directory first (removes
  // the current directory from the search path).
  const BOOL set_dir_ok = SetDllDirectoryW(dir.c_str());
  kgcore::Log("startup", "SetDllDirectory(%ls) -> %s", dir.c_str(), set_dir_ok ? "OK" : "GAGAL");

  // ---- onnxruntime.dll (must come from OUR folder, not System32) ----
  DWORD err = 0;
  std::wstring ort_path;
  HMODULE ort = LoadFromExeDir(dir, L"onnxruntime.dll", &ort_path, &err);
  if (!ort) {
    result.detail = MissingDllDetail(dir, L"onnxruntime.dll", err);
    kgcore::Log("startup", "onnxruntime: GAGAL dimuat (kode %lu)", err);
    return result;
  }
  result.ort_module = ort_path;
  kgcore::Log("startup", "onnxruntime: dimuat dari %ls", ort_path.c_str());

  const OrtApiBase* base = OrtGetApiBase();
  if (!base) {
    result.detail = L"OrtGetApiBase() mengembalikan nullptr — onnxruntime yang dimuat tidak "
                    L"cocok/rusak. Versi yang diharapkan: 1.31.1.";
    kgcore::Log("startup", "onnxruntime: OrtGetApiBase() == nullptr");
    return result;
  }
  const char* vs = base->GetVersionString();
  result.ort_version = vs ? vs : "(tidak diketahui)";
  kgcore::Log("startup", "onnxruntime: OrtGetApiBase OK — version string '%s'",
              result.ort_version.c_str());
  if (strncmp(result.ort_version.c_str(), "1.31.", 5) != 0) {
    kgcore::Log("startup", "PERINGATAN: versi ORT '%s' berbeda dari yang dipin CMake (1.31.1)",
                result.ort_version.c_str());
  }

  // ---- gpupixel.dll ----
  std::wstring gpu_path;
  HMODULE gpu = LoadFromExeDir(dir, L"gpupixel.dll", &gpu_path, &err);
  if (!gpu) {
    result.detail = MissingDllDetail(dir, L"gpupixel.dll", err);
    kgcore::Log("startup", "gpupixel: GAGAL dimuat (kode %lu)", err);
    return result;
  }
  result.gpupixel_module = gpu_path;
  kgcore::Log("startup", "gpupixel: dimuat dari %ls", gpu_path.c_str());

  result.all_ok = true;
  return result;
}

void ShowFatalAndExit(const wchar_t* context, const std::wstring& detail, int exit_code) {
  kgcore::Log("fatal", "%ls: %ls", context, detail.c_str());
  kgcore::Log("fatal", "log: %ls", g_log_path[0] ? g_log_path : L"(log file tidak aktif)");
  std::wstring msg = L"KameraGeulis tidak bisa dimulai.\n\n";
  msg += L"Tahap: ";
  msg += context;
  msg += L"\n\n";
  msg += detail;
  msg += L"\n\nLog: ";
  msg += (g_log_path[0] ? g_log_path : L"(tidak tersedia)");
  MessageBoxW(nullptr, msg.c_str(), L"KameraGeulis — Gagal Startup",
              MB_ICONERROR | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
  kgcore::CloseLogFile();
  ExitProcess(static_cast<UINT>(exit_code));
}

}  // namespace kgcore
