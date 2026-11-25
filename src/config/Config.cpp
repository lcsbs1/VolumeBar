#include "config/Config.h"

#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <vector>

#include "audio/AudioEngine.h"
#include "util/Strings.h"

namespace {

constexpr wchar_t kRunRegPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"VolumeBar";

// Minimal recursive-descent JSON value & parser (no exceptions, zero external deps).
struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::map<std::string, JsonValue> objVal;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : s_(src) {}

    bool Parse(JsonValue* out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        return pos_ == s_.size();
    }

private:
    void SkipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool ParseValue(JsonValue* out) {
        SkipWs();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '{') return ParseObject(out);
        if (c == '[') return ParseArray(out);
        if (c == '"') {
            out->type = JsonValue::String;
            return ParseString(&out->strVal);
        }
        if (c == 't' && s_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out->type = JsonValue::Bool;
            out->boolVal = true;
            return true;
        }
        if (c == 'f' && s_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out->type = JsonValue::Bool;
            out->boolVal = false;
            return true;
        }
        if (c == 'n' && s_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out->type = JsonValue::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseNumber(out);
        }
        return false;
    }

    bool ParseObject(JsonValue* out) {
        out->type = JsonValue::Object;
        out->objVal.clear();
        ++pos_; // '{'
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return false;
            std::string key;
            if (!ParseString(&key)) return false;
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            ++pos_;
            JsonValue val;
            if (!ParseValue(&val)) return false;
            out->objVal[std::move(key)] = std::move(val);
            SkipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
        return false;
    }

    bool ParseArray(JsonValue* out) {
        out->type = JsonValue::Array;
        out->arrVal.clear();
        ++pos_; // '['
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (pos_ < s_.size()) {
            JsonValue item;
            if (!ParseValue(&item)) return false;
            out->arrVal.push_back(std::move(item));
            SkipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
        return false;
    }

    static void AppendUtf8(std::string& dst, unsigned int cp) {
        if (cp <= 0x7F) {
            dst.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            dst.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            dst.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseString(std::string* out) {
        out->clear();
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                char esc = s_[pos_++];
                switch (esc) {
                    case '"':  out->push_back('"'); break;
                    case '\\': out->push_back('\\'); break;
                    case '/':  out->push_back('/'); break;
                    case 'b':  out->push_back('\b'); break;
                    case 'f':  out->push_back('\f'); break;
                    case 'n':  out->push_back('\n'); break;
                    case 'r':  out->push_back('\r'); break;
                    case 't':  out->push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned int cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                            else return false;
                        }
                        AppendUtf8(*out, cp);
                        break;
                    }
                    default: return false;
                }
            } else {
                out->push_back(c);
            }
        }
        return false;
    }

    bool ParseNumber(JsonValue* out) {
        size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
        }
        std::string token(s_.substr(start, pos_ - start));
        char* endPtr = nullptr;
        double v = std::strtod(token.c_str(), &endPtr);
        if (endPtr == token.c_str()) return false;
        out->type = JsonValue::Number;
        out->numVal = v;
        return true;
    }

    std::string_view s_;
    size_t pos_ = 0;
};

std::string EscapeJsonString(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8]{};
                    wsprintfA(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

bool ReadWholeFile(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(f);
        return false;
    }
    std::string buf(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    bool ok = (size.QuadPart == 0) ||
              (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) && read == buf.size());
    CloseHandle(f);
    if (ok) *out = std::move(buf);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const std::string& content) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring dir = path.substr(0, slash);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(f);
    return ok != FALSE && written == content.size();
}

} // namespace

std::wstring ConfigManager::GetConfigPath() const {
    PWSTR appData = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        base = appData;
        CoTaskMemFree(appData);
    } else {
        wchar_t envBuf[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"APPDATA", envBuf, MAX_PATH) > 0) {
            base = envBuf;
        }
    }
    if (base.empty()) return L"config.json";
    return base + L"\\VolumeBar\\config.json";
}

void ConfigManager::SetDefaults() {
    cfg_ = AppConfig{};
    cfg_.maxWidth = 360;
    cfg_.scrollStep = 2;
    cfg_.openPanelHotkey = L"Ctrl+Alt+V";

    VolumeProfile jogo;
    jogo.hotkey = L"Ctrl+Alt+1";
    jogo.apps[L"spotify.exe"] = 20;
    jogo.apps[L"discord.exe"] = 80;
    cfg_.profiles[L"Jogo"] = std::move(jogo);

    VolumeProfile musica;
    musica.hotkey = L"Ctrl+Alt+2";
    musica.apps[L"spotify.exe"] = 100;
    musica.apps[L"discord.exe"] = 40;
    cfg_.profiles[L"Música"] = std::move(musica);
}

bool ConfigManager::Load() {
    SetDefaults();
    std::wstring path = GetConfigPath();
    std::string content;
    if (!ReadWholeFile(path, &content)) {
        Save();
        return true;
    }
    if (!ParseJson(content)) {
        return false;
    }
    return true;
}

bool ConfigManager::Save() const {
    return WriteWholeFile(GetConfigPath(), SerializeJson());
}

bool ConfigManager::ParseJson(const std::string& utf8) {
    std::string_view sv = utf8;
    if (sv.size() >= 3 && static_cast<unsigned char>(sv[0]) == 0xEF &&
        static_cast<unsigned char>(sv[1]) == 0xBB && static_cast<unsigned char>(sv[2]) == 0xBF) {
        sv.remove_prefix(3);
    }

    JsonValue root;
    JsonParser parser(sv);
    if (!parser.Parse(&root) || root.type != JsonValue::Object) {
        return false;
    }

    auto itW = root.objVal.find("maxWidth");
    if (itW != root.objVal.end() && itW->second.type == JsonValue::Number) {
        int w = static_cast<int>(std::lround(itW->second.numVal));
        cfg_.maxWidth = (std::max)(120, (std::min)(1200, w));
    }

    auto itStep = root.objVal.find("scrollStep");
    if (itStep != root.objVal.end() && itStep->second.type == JsonValue::Number) {
        int s = static_cast<int>(std::lround(itStep->second.numVal));
        cfg_.scrollStep = (std::max)(1, (std::min)(25, s));
    }

    auto itMulti = root.objVal.find("showOnAllMonitors");
    if (itMulti != root.objVal.end() && itMulti->second.type == JsonValue::Bool) {
        cfg_.showOnAllMonitors = itMulti->second.boolVal;
    }

    auto itHk = root.objVal.find("hotkeys");
    if (itHk != root.objVal.end() && itHk->second.type == JsonValue::Object) {
        auto itOpen = itHk->second.objVal.find("openPanel");
        if (itOpen != itHk->second.objVal.end() && itOpen->second.type == JsonValue::String) {
            cfg_.openPanelHotkey = util::Utf8ToWide(itOpen->second.strVal);
        }
    }

    auto itProf = root.objVal.find("profiles");
    if (itProf != root.objVal.end() && itProf->second.type == JsonValue::Object) {
        cfg_.profiles.clear();
        for (const auto& kv : itProf->second.objVal) {
            if (kv.second.type != JsonValue::Object) continue;
            VolumeProfile prof;
            auto itPhk = kv.second.objVal.find("hotkey");
            if (itPhk != kv.second.objVal.end() && itPhk->second.type == JsonValue::String) {
                prof.hotkey = util::Utf8ToWide(itPhk->second.strVal);
            }
            auto itApps = kv.second.objVal.find("apps");
            if (itApps != kv.second.objVal.end() && itApps->second.type == JsonValue::Object) {
                for (const auto& akv : itApps->second.objVal) {
                    if (akv.second.type != JsonValue::Number) continue;
                    int v = static_cast<int>(std::lround(akv.second.numVal));
                    v = (std::max)(0, (std::min)(100, v));
                    prof.apps[util::ToLower(util::Utf8ToWide(akv.first))] = v;
                }
            }
            cfg_.profiles[util::Utf8ToWide(kv.first)] = std::move(prof);
        }
    }
    return true;
}

std::string ConfigManager::SerializeJson() const {
    std::string out;
    out += "{\n";
    out += "  \"maxWidth\": " + std::to_string(cfg_.maxWidth) + ",\n";
    out += "  \"scrollStep\": " + std::to_string(cfg_.scrollStep) + ",\n";
    out += "  \"showOnAllMonitors\": " + std::string(cfg_.showOnAllMonitors ? "true" : "false") + ",\n";
    out += "  \"hotkeys\": { \"openPanel\": " + EscapeJsonString(util::WideToUtf8(cfg_.openPanelHotkey)) + " },\n";
    out += "  \"profiles\": {\n";

    size_t pi = 0;
    for (const auto& kv : cfg_.profiles) {
        out += "    " + EscapeJsonString(util::WideToUtf8(kv.first)) + ": { ";
        out += "\"hotkey\": " + EscapeJsonString(util::WideToUtf8(kv.second.hotkey)) + ", \"apps\": { ";
        size_t ai = 0;
        for (const auto& akv : kv.second.apps) {
            out += EscapeJsonString(util::WideToUtf8(akv.first)) + ": " + std::to_string(akv.second);
            if (++ai < kv.second.apps.size()) out += ", ";
        }
        out += " } }";
        if (++pi < cfg_.profiles.size()) out += ",";
        out += "\n";
    }

    out += "  }\n";
    out += "}\n";
    return out;
}

bool ConfigManager::ApplyProfile(const std::wstring& profileName, AudioEngine& engine) const {
    auto it = cfg_.profiles.find(profileName);
    if (it == cfg_.profiles.end()) return false;
    bool anyApplied = false;
    for (const auto& kv : it->second.apps) {
        std::wstring resolved;
        if (engine.FindAppId(kv.first, &resolved)) {
            if (engine.SetAppVolume(resolved, static_cast<float>(kv.second) / 100.0f)) {
                anyApplied = true;
            }
        }
    }
    return anyApplied;
}

bool ConfigManager::SaveCurrentAsProfile(const std::wstring& profileName, const AudioEngine& engine) {
    std::wstring trimmed = util::Trim(profileName);
    if (trimmed.empty()) return false;

    VolumeProfile prof;
    auto existing = cfg_.profiles.find(trimmed);
    if (existing != cfg_.profiles.end()) {
        prof.hotkey = existing->second.hotkey;
    }

    auto apps = engine.GetApps();
    for (const auto& a : apps) {
        if (a.isSystemSounds) continue;
        int pct = static_cast<int>(std::lround(a.volume * 100.0f));
        pct = (std::max)(0, (std::min)(100, pct));
        prof.apps[a.appId] = pct;
    }

    cfg_.profiles[trimmed] = std::move(prof);
    return Save();
}

bool ConfigManager::ParseHotkey(std::wstring_view text, UINT* outMods, UINT* outVk) {
    if (!outMods || !outVk) return false;
    std::wstring cleaned = util::Trim(text);
    if (cleaned.empty()) return false;

    UINT mods = 0;
    UINT vk = 0;
    size_t start = 0;
    while (start <= cleaned.size()) {
        size_t plus = cleaned.find(L'+', start);
        std::wstring part = util::ToLower(util::Trim(
            plus == std::wstring::npos ? std::wstring_view(cleaned).substr(start)
                                       : std::wstring_view(cleaned).substr(start, plus - start)));
        if (!part.empty()) {
            if (part == L"ctrl" || part == L"control") mods |= MOD_CONTROL;
            else if (part == L"alt") mods |= MOD_ALT;
            else if (part == L"shift") mods |= MOD_SHIFT;
            else if (part == L"win" || part == L"super") mods |= MOD_WIN;
            else if (part.size() == 1) {
                wchar_t ch = part[0];
                if (ch >= L'a' && ch <= L'z') vk = static_cast<UINT>('A' + (ch - L'a'));
                else if (ch >= L'0' && ch <= L'9') vk = static_cast<UINT>('0' + (ch - L'0'));
            } else if (part[0] == L'f' && part.size() <= 3) {
                int fnum = std::wcstol(part.c_str() + 1, nullptr, 10);
                if (fnum >= 1 && fnum <= 12) vk = static_cast<UINT>(VK_F1 + (fnum - 1));
            }
        }
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }

    if (vk == 0) return false;
    *outMods = mods | MOD_NOREPEAT;
    *outVk = vk;
    return true;
}

bool ConfigManager::IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    bool exists = (RegQueryValueExW(key, kRunValueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
                   (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t));
    RegCloseKey(key);
    return exists;
}

bool ConfigManager::SetAutostart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    if (enable) {
        wchar_t exePath[MAX_PATH * 2]{};
        DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(sizeof(exePath) / sizeof(wchar_t)));
        if (len > 0) {
            std::wstring quoted = L"\"";
            quoted.append(exePath, len);
            quoted += L"\"";
            ok = (RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                                 reinterpret_cast<const BYTE*>(quoted.c_str()),
                                 static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
        }
    } else {
        LSTATUS st = RegDeleteValueW(key, kRunValueName);
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}
