#pragma once
#include <windows.h>
#include <stream_names.h>
#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <chrono>

namespace redesk::host::windows {

// Native text-only clipboard for the interactive probe. All operations run on
// its owner thread. No initial clipboard contents are read or transmitted.
class ClipboardProbe {
    struct Open {
        bool ok;
        explicit Open(HWND owner) : ok(OpenClipboard(owner) != FALSE) {}
        ~Open() { if (ok) CloseClipboard(); }
    };
    struct Memory {
        HGLOBAL handle;
        explicit Memory(HGLOBAL h) : handle(h) {}
        ~Memory() { if (handle) GlobalFree(handle); }
    };
    struct Locked {
        HGLOBAL handle;
        void* data;
        explicit Locked(HGLOBAL h) : handle(h), data(GlobalLock(h)) {}
        ~Locked() { if (data) GlobalUnlock(handle); }
    };
    using Clock = std::chrono::steady_clock;
public:
    ClipboardProbe() {
        // EmptyClipboard needs a real owner window; nullptr can make
        // SetClipboardData fail. No keyboard hooks or visible UI are created.
        owner_ = CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,
                                 nullptr,GetModuleHandleW(nullptr),nullptr);
        if (!owner_) throw std::runtime_error("Cannot create clipboard owner");
        seen_ = GetClipboardSequenceNumber();
    }
    ~ClipboardProbe() { if (owner_) DestroyWindow(owner_); }
    ClipboardProbe(const ClipboardProbe&) = delete;
    ClipboardProbe& operator=(const ClipboardProbe&) = delete;

    // The caller checks authorization immediately before each OS operation.
    // Remote writes supersede older unsent values and never echo back.
    void receive(std::string value) {
        if (value.empty() || value.size() > crossdesk::kMaxClipboardBytes ||
            value.find('\0') != std::string::npos) return;
        const int n = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
                         value.data(),static_cast<int>(value.size()),nullptr,0);
        if (n <= 0) return;
        std::wstring wide(static_cast<std::size_t>(n),L'\0');
        if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
                               static_cast<int>(value.size()),wide.data(),n) != n) return;
        incoming_ = std::move(wide);
        outgoing_.reset();
        write_deadline_ = Clock::now()+std::chrono::seconds(2);
    }
    template<class Active,class Send>
    void tick(Active active,Send send) {
        const auto now = Clock::now();
        if (now < next_poll_ || !active()) return;
        next_poll_ = now+std::chrono::milliseconds(100);
        if (incoming_) {
            if (now >= write_deadline_) {
                incoming_.reset();
                seen_ = GetClipboardSequenceNumber();
                return;
            }
            if (active() && write(*incoming_)) incoming_.reset();
            return;
        }
        if (GetClipboardSequenceNumber() != seen_ && active()) read_changed();
        if (outgoing_ && active() && send(*outgoing_)) outgoing_.reset();
    }
private:
    bool write(const std::wstring& text) {
        Memory memory(GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t)));
        if (!memory.handle) return false;
        {
            Locked lock(memory.handle);
            if (!lock.data) return false;
            std::memcpy(lock.data,text.c_str(),(text.size()+1)*sizeof(wchar_t));
        }
        Open open(owner_);
        if (!open.ok) return false;
        if (!EmptyClipboard()) return false;
        if (!SetClipboardData(CF_UNICODETEXT,memory.handle)) {
            // Clipboard may already be empty; do not repeatedly empty it.
            seen_=GetClipboardSequenceNumber();
            return true;
        }
        memory.handle=nullptr; // Windows owns this memory after success.
        seen_=GetClipboardSequenceNumber();
        return true;
    }
    void read_changed() {
        Open open(owner_);
        if (!open.ok) return; // retry a busy clipboard on next poll
        if (GetClipboardOwner()==owner_) {
            seen_=GetClipboardSequenceNumber();
            outgoing_.reset();
            return;
        }
        outgoing_.reset();
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            seen_=GetClipboardSequenceNumber();
            return;
        }
        const auto handle=GetClipboardData(CF_UNICODETEXT);
        if (!handle) return;
        const auto bytes=GlobalSize(handle);
        if (bytes < sizeof(wchar_t)) { seen_=GetClipboardSequenceNumber(); return; }
        Locked lock(handle);
        if (!lock.data) return;
        seen_=GetClipboardSequenceNumber();
        const auto* begin=static_cast<const wchar_t*>(lock.data);
        // Bound scanning even when an untrusted clipboard advertises a huge
        // global allocation; also require a terminating UTF-16 NUL.
        const auto limit=(std::min)(bytes/sizeof(wchar_t),crossdesk::kMaxClipboardBytes+1);
        const auto* end=std::find(begin,begin+limit,L'\0');
        if (end==begin || end==begin+limit) return;
        const int units=static_cast<int>(end-begin);
        const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,begin,units,
                                       nullptr,0,nullptr,nullptr);
        if (n<=0 || static_cast<std::size_t>(n)>crossdesk::kMaxClipboardBytes) return;
        std::string text(static_cast<std::size_t>(n),'\0');
        if (WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,begin,units,
                               text.data(),n,nullptr,nullptr)==n) outgoing_=std::move(text);
    }
    HWND owner_=nullptr;
    DWORD seen_=0;
    Clock::time_point next_poll_{},write_deadline_{};
    std::optional<std::wstring> incoming_;
    std::optional<std::string> outgoing_;
};
}
