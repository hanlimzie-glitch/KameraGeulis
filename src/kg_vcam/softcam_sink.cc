#include "softcam_sink.h"

#include <windows.h>

#include <string>

namespace kgcore {
namespace {

constexpr wchar_t kSoftcamClsidPath[] =
    L"SOFTWARE\\Classes\\CLSID\\{AEF3B972-5FA5-4647-9571-358EB472BC9E}\\InprocServer32";
constexpr wchar_t kDShowCategoryInstancePath[] =
    L"SOFTWARE\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\"
    L"KameraGeulis Camera";

bool FileExistsW(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      out.data(), n);
  return out;
}

bool CheckRegClsid(REGSAM view_flag, bool* has_valid_dll_path = nullptr) {
  if (has_valid_dll_path) *has_valid_dll_path = false;
  HKEY hkey = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSoftcamClsidPath, 0,
                    KEY_READ | view_flag, &hkey) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t buf[MAX_PATH * 2]{};
  DWORD size = sizeof(buf);
  DWORD type = 0;
  const LSTATUS rc =
      RegQueryValueExW(hkey, nullptr, nullptr, &type,
                       reinterpret_cast<LPBYTE>(buf), &size);
  RegCloseKey(hkey);
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return false;
  }
  const std::wstring reg_path(buf);
  const bool dll_ok = FileExistsW(reg_path);
  if (has_valid_dll_path) *has_valid_dll_path = dll_ok;

  HKEY hcat = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDShowCategoryInstancePath, 0,
                    KEY_READ | view_flag, &hcat) != ERROR_SUCCESS) {
    return false;
  }
  RegCloseKey(hcat);
  return dll_ok;
}

}  // namespace

SoftcamSink::SoftcamSink(std::string dll_directory)
    : dll_directory_(std::move(dll_directory)) {}

SoftcamSink::~SoftcamSink() { Close(); }

bool SoftcamSink::IsRegistered(std::string* detail_out) const {
  const std::wstring wdir = Utf8ToWide(dll_directory_);
  const std::wstring x64_dll =
      wdir.empty() ? L"softcam.dll" : (wdir + L"\\softcam.dll");
  const std::wstring x86_dll =
      wdir.empty() ? L"win32\\softcam.dll" : (wdir + L"\\win32\\softcam.dll");

  const bool local_x64 = FileExistsW(x64_dll);
  const bool local_x86 = FileExistsW(x86_dll);

  bool x64_dll_ok = false;
  const bool reg_x64 = CheckRegClsid(KEY_WOW64_64KEY, &x64_dll_ok);
  bool x86_dll_ok = false;
  const bool reg_x86 = CheckRegClsid(KEY_WOW64_32KEY, &x86_dll_ok);

  if (detail_out) {
    if (!local_x64) {
      *detail_out = "softcam.dll tidak ditemukan di folder aplikasi";
    } else if (!reg_x64 && !reg_x86) {
      *detail_out = "belum terdaftar di Windows Registry (jalankan install_vcam.bat 1x sebagai Admin)";
    } else if (reg_x64 && reg_x86) {
      *detail_out = "terdaftar (x64 + Win32)";
    } else if (reg_x64) {
      *detail_out = local_x86
                        ? "terdaftar x64 saja (daftar ulang via install_vcam.bat untuk app 32-bit seperti Discord)"
                        : "terdaftar (x64)";
    } else {
      *detail_out = "terdaftar (Win32 saja)";
    }
  }
  // Pada aplikasi 64-bit, kita membutuhkan registrasi x64 + softcam.dll lokal
  // agar kamera terlihat oleh host 64-bit & LoadLibrary berhasil.
  return local_x64 && reg_x64;
}

bool SoftcamSink::Open(int width, int height, float fps, std::string* err_out) {
  if (!sender_.Open(width, height, fps, dll_directory_)) {
    if (err_out) *err_out = sender_.LastError();
    return false;
  }
  return true;
}

void SoftcamSink::Close() { sender_.Close(); }

bool SoftcamSink::SendFrame(const uint8_t* data, size_t bytes, int width,
                            int height, int64_t /*timestamp_us*/) {
  if (!sender_.IsOpen()) return false;
  if (width != sender_.Width() || height != sender_.Height()) return false;
  return sender_.SendBgr24(data, bytes);
}

int SoftcamSink::ConsumerCount() const {
  if (!sender_.IsOpen()) return -1;
  return sender_.ConsumerConnected() ? 1 : 0;
}

}  // namespace kgcore
