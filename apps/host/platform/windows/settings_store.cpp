#include "settings_store.h"
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <array>
#include <memory>
#include <vector>

namespace redesk::host::windows {
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void close() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
};
using Local = std::unique_ptr<void, decltype(&LocalFree)>;
std::vector<BYTE> user_token() {
    HANDLE raw = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "Cannot open process token");
    Handle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    require(size != 0, "Cannot query user SID");
    std::vector<BYTE> result(size);
    require(GetTokenInformation(token.value, TokenUser, result.data(), size, &size), "Cannot read user SID");
    return result;
}
PSID sid_of(const std::vector<BYTE>& user) {
    return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
}
bool allowed(PSID sid, StoreMode mode, PSID user) {
    if (!sid || !IsValidSid(sid)) return false;
    if (mode == StoreMode::Portable) return EqualSid(sid, user) != FALSE;
    return IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
}
void require_access(StoreMode mode, PSID user) {
    if (mode == StoreMode::Portable) return;
    require(mode == StoreMode::Installed, "Invalid settings store mode");
    if (IsWellKnownSid(user, WinLocalSystemSid)) return;
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(sid);
    BOOL member = FALSE;
    require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &size) &&
            CheckTokenMembership(nullptr, sid, &member) && member,
            "Installed settings require an elevated administrator or SYSTEM");
}
Local descriptor(StoreMode mode, PSID user, bool directory) {
    const std::wstring flags = directory ? L"OICI" : L"";
    std::wstring sddl;
    if (mode == StoreMode::Portable) {
        LPWSTR raw = nullptr;
        require(ConvertSidToStringSidW(user, &raw), "Cannot format user SID");
        Local guard(raw, LocalFree);
        const std::wstring sid(raw);
        sddl = L"O:" + sid + L"D:P(A;" + flags + L";FA;;;" + sid + L")";
    } else {
        sddl = L"O:BAD:P(A;" + flags + L";FA;;;SY)(A;" + flags + L";FA;;;BA)";
    }
    PSECURITY_DESCRIPTOR raw = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
            &raw, nullptr), "Cannot construct private ACL");
    return Local(raw, LocalFree);
}
void check(HANDLE h, StoreMode mode, PSID user, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(h, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (!!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory) &&
            (directory || info.nNumberOfLinks == 1), "Unsafe host settings path");
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    require(GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &acl, nullptr, &raw) == ERROR_SUCCESS, "Cannot inspect settings ACL");
    Local guard(raw, LocalFree);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    require(allowed(owner, mode, user) && acl && acl->AceCount &&
            GetSecurityDescriptorControl(raw, &control, &revision) &&
            (control & SE_DACL_PROTECTED), "Settings owner or ACL is unsafe");
    bool has_user = false, has_system = false, has_admin = false;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* entry = nullptr;
        require(GetAce(acl, i, &entry), "Cannot inspect settings ACL entry");
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
        const BYTE expected = directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0;
        require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                ace->Header.AceFlags == expected && ace->Mask == FILE_ALL_ACCESS,
                "Unexpected settings ACL entry");
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        require(allowed(sid, mode, user), "Settings ACL grants access to an unexpected account");
        has_user |= EqualSid(sid, user) != FALSE;
        has_system |= IsWellKnownSid(sid, WinLocalSystemSid) != FALSE;
        has_admin |= IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != FALSE;
    }
    require(mode == StoreMode::Portable ? has_user : has_system && has_admin,
            "Required settings permissions are missing");
}
HANDLE open_root(const std::filesystem::path& root, StoreMode mode, PSID user, bool create) {
    if (create) {
        auto security = descriptor(mode, user, true);
        SECURITY_ATTRIBUTES sa{sizeof(sa), security.get(), FALSE};
        if (!CreateDirectoryW(root.c_str(), &sa))
            require(GetLastError() == ERROR_ALREADY_EXISTS, "Cannot create settings directory");
    }
    HANDLE raw = CreateFileW(root.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        if (!create && GetLastError() == ERROR_FILE_NOT_FOUND) return raw;
        if (!create && GetLastError() == ERROR_PATH_NOT_FOUND) return raw;
        throw std::runtime_error("Cannot open settings directory");
    }
    try { check(raw, mode, user, true); } catch (...) { CloseHandle(raw); throw; }
    return raw;
}
HANDLE open_file(const std::filesystem::path& file, StoreMode mode, PSID user) {
    HANDLE raw = CreateFileW(file.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return raw;
        throw std::runtime_error("Cannot open host settings file");
    }
    try { check(raw, mode, user, false); } catch (...) { CloseHandle(raw); throw; }
    return raw;
}
constexpr DWORD max_ciphertext = static_cast<DWORD>(snapshot::max_bytes + 65536);
std::wstring random_name() {
    std::array<unsigned char, 16> bytes{};
    require(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0, "Cannot generate temporary filename");
    std::wstring result = L".settings-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (auto b : bytes) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}
}

std::filesystem::path settings_directory(StoreMode mode) {
    require(mode == StoreMode::Portable || mode == StoreMode::Installed, "Invalid settings store mode");
    PWSTR raw = nullptr;
    require(SUCCEEDED(SHGetKnownFolderPath(mode == StoreMode::Portable ?
        FOLDERID_LocalAppData : FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw)),
        "Cannot resolve host settings directory");
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> guard(raw, CoTaskMemFree);
    return std::filesystem::path(raw) / L"ReDeskHost";
}
std::optional<HostSettings> load_settings(StoreMode mode) {
    const auto user = user_token();
    const auto sid = sid_of(user);
    require_access(mode, sid);
    const auto root = settings_directory(mode);
    Handle directory(open_root(root, mode, sid, false));
    if (directory.value == INVALID_HANDLE_VALUE) return std::nullopt;
    Handle file(open_file(root / L"settings.dat", mode, sid));
    if (file.value == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.value, &size) && size.QuadPart > 0 &&
            size.QuadPart <= max_ciphertext, "Invalid host settings file size");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
            read == bytes.size(), "Cannot read host settings");
    return unprotect_settings(bytes);
}
void save_settings(StoreMode mode, const HostSettings& settings, SaveMode save_mode) {
    require(save_mode == SaveMode::Replace || save_mode == SaveMode::CreateOnly, "Invalid save mode");
    const auto user = user_token();
    const auto sid = sid_of(user);
    require_access(mode, sid);
    const auto root = settings_directory(mode);
    Handle directory(open_root(root, mode, sid, true));
    const auto destination = root / L"settings.dat";
    {
        Handle existing(open_file(destination, mode, sid));
        if (existing.value != INVALID_HANDLE_VALUE)
            require(save_mode == SaveMode::Replace, "Installed settings already exist");
    }
    const auto ciphertext = protect_settings(settings, mode == StoreMode::Portable ?
        ProtectionScope::CurrentUser : ProtectionScope::LocalMachine);
    const auto temporary = root / random_name();
    auto security = descriptor(mode, sid, false);
    SECURITY_ATTRIBUTES sa{sizeof(sa), security.get(), FALSE};
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE | READ_CONTROL, 0, &sa,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Cannot create temporary settings file");
    try {
        check(file.value, mode, sid, false);
        DWORD written = 0;
        require(WriteFile(file.value, ciphertext.data(), static_cast<DWORD>(ciphertext.size()),
                &written, nullptr) && written == ciphertext.size() && FlushFileBuffers(file.value),
                "Cannot persist encrypted host settings");
        file.close();
        DWORD flags = MOVEFILE_WRITE_THROUGH;
        if (save_mode == SaveMode::Replace) flags |= MOVEFILE_REPLACE_EXISTING;
        require(MoveFileExW(temporary.c_str(), destination.c_str(), flags),
                "Cannot commit host settings");
    } catch (...) {
        file.close();
        DeleteFileW(temporary.c_str());
        throw;
    }
}
}
