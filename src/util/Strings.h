#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace util {

inline std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

inline std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

inline std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty()) {
        CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    return out;
}

inline std::wstring Trim(std::wstring_view s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'\r' || s[start] == L'\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r' || s[end - 1] == L'\n')) {
        --end;
    }
    return std::wstring(s.substr(start, end - start));
}

inline std::wstring BaseName(std::wstring_view path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring_view::npos) return std::wstring(path);
    return std::wstring(path.substr(pos + 1));
}

inline std::wstring StripExtension(std::wstring_view filename) {
    size_t dot = filename.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0) return std::wstring(filename);
    return std::wstring(filename.substr(0, dot));
}

inline bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

inline void InitCliConsole() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    bool hasOut = (hOut != nullptr && hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) != FILE_TYPE_UNKNOWN);
    bool hasErr = (hErr != nullptr && hErr != INVALID_HANDLE_VALUE && GetFileType(hErr) != FILE_TYPE_UNKNOWN);
    if (!hasOut && !hasErr) {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
}

inline void WriteHandleText(DWORD stdHandleId, std::wstring_view text) {
    HANDLE h = GetStdHandle(stdHandleId);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return;
    DWORD type = GetFileType(h);
    if (type == FILE_TYPE_UNKNOWN) return;

    if (type == FILE_TYPE_CHAR) {
        DWORD written = 0;
        WriteConsoleW(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }

    std::string utf8 = WideToUtf8(text);
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

inline void WriteOut(std::wstring_view text) {
    WriteHandleText(STD_OUTPUT_HANDLE, text);
}

inline void WriteErr(std::wstring_view text) {
    WriteHandleText(STD_ERROR_HANDLE, text);
}

} // namespace util
