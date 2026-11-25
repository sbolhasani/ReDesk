#pragma once
#include "signal_peer.h"
#include "interactive_desktop.h"
#include <file_transfer_core.h>
#include <windows.h>
#include <openssl/rand.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace redesk::host::windows {
namespace fs = std::filesystem;

// Deliberately limited legacy file-transfer probe. No remote filesystem
// listing, arbitrary remote destination, execution, or directory RPC.
class FileProbe {
public:
    static constexpr std::uint64_t kMaxFile=256ull*1024*1024;
    static constexpr std::uint64_t kMaxTotal=1024ull*1024*1024;
    explicit FileProbe(SignalPeer& peer,const fs::path& root,const fs::path& outgoing)
        : peer_(peer),directory_(make_directory(root)),
          session_([this](const char* stream,const char* data,std::size_t size) {
                return active() ? peer_.send_file_data(stream,data,size) : -1;
            },[this](const std::string&,double value,bool sending,const std::string&) {
                if (sending && (value==1 || value<0))
                    std::cout << "File send: " << (value==1?"completed":"failed or cancelled") << std::endl;
            },{false,false,4},[this](const crossdesk::file_transfer::ReceiveState& state) {
                using crossdesk::file_transfer::ReceiveStatus;
                if (state.status==ReceiveStatus::Receiving) return;
                if (auto it=seen_.find(state.id);it!=seen_.end()) it->second.done=true;
                if (state.status==ReceiveStatus::Completed) {
                    ++completed_;
                    std::cout << "File receive completed: " << state.received << " bytes" << std::endl;
                } else std::cout << "File receive failed or cancelled" << std::endl;
            }) {
        session_.SetDirectory(directory_);
        std::wcout << L"File receive directory: " << directory_.wstring() << std::endl;
        if (!outgoing.empty()) {
            validate_path(outgoing,false);
            const auto u8=outgoing.filename().u8string();
            const std::string name(u8.begin(),u8.end());
            std::error_code ec;
            const auto size=fs::file_size(outgoing,ec);
            if (ec || size>kMaxFile || !safe_name(name) ||
                !session_.Start(outgoing,name))
                throw std::runtime_error("Invalid or inaccessible local send file");
            std::cout << "Locally selected file queued: " << size << " bytes" << std::endl;
        }
    }
    ~FileProbe() { try { session_.Cancel(); } catch (...) {} }
    FileProbe(const FileProbe&)=delete;
    FileProbe& operator=(const FileProbe&)=delete;

    bool active() const {
        return !failed_ && peer_.view_active() &&
            !peer_.status().file_queue_failed &&
            crossdesk::IsCurrentSessionUserDesktopActive();
    }
    void stop() {
        session_.Cancel();
        peer_.disable_files();
        failed_=true;
    }
    void tick() {
        if (failed_) return;
        if (!active()) { stop(); return; }
        try {
            // Recheck the isolated receive directory before processing data.
            validate_path(directory_,true);
            for (const auto& packet:peer_.take_files()) {
                if (!active()) { stop(); return; }
                if (packet.stream==crossdesk::kFileFeedbackStream) {
                    session_.Ack(packet.payload.data(),packet.payload.size());
                    continue;
                }
                if (!receive(packet.payload)) {
                    std::cout << "File transfer rejected: invalid packet or probe limit" << std::endl;
                    stop(); return;
                }
            }
            // Bound disk and transport work per iteration; let video/input run.
            for (unsigned i=0;i<4 && active();++i)
                if (!session_.Pump()) break;
        } catch (...) {
            std::cout << "File transfer stopped: local filesystem or protocol error" << std::endl;
            stop();
        }
    }
    std::size_t completed() const { return completed_; }
private:
    struct Seen { std::uint64_t total; bool done=false; };
    static void validate_path(const fs::path& p,bool directory) {
        // Accept drive-letter absolute local paths only, not device/UNC paths,
        // alternate streams, parent traversals or junction/symlink ancestors.
        const auto root=p.root_name().wstring();
        if (!p.is_absolute() || root.size()!=2 || root[1]!=L':' ||
            GetDriveTypeW(p.root_path().c_str())!=DRIVE_FIXED)
            throw std::runtime_error("Expected an absolute path on a local fixed drive");
        fs::path current=p.root_path();
        for (const auto& part:p.relative_path()) {
            const auto value=part.wstring();
            if (value.empty() || value==L"." || value==L".." ||
                value.find_first_of(L":*?\"<>|")!=std::wstring::npos ||
                value.back()==L'.' || value.back()==L' ')
                throw std::runtime_error("Invalid file path");
            current/=part;
            const DWORD attr=GetFileAttributesW(current.c_str());
            if (attr==INVALID_FILE_ATTRIBUTES || (attr&FILE_ATTRIBUTE_REPARSE_POINT) ||
                (current!=p && !(attr&FILE_ATTRIBUTE_DIRECTORY)))
                throw std::runtime_error("Unavailable or redirected file path");
        }
        const DWORD attr=GetFileAttributesW(p.c_str());
        if (attr==INVALID_FILE_ATTRIBUTES || (attr&FILE_ATTRIBUTE_REPARSE_POINT) ||
            ((attr&FILE_ATTRIBUTE_DIRECTORY)!=0)!=directory)
            throw std::runtime_error("Unexpected file path type");
    }
    static fs::path make_directory(const fs::path& root) {
        validate_path(root,true);
        unsigned char random[16]{};
        if (RAND_bytes(random,sizeof(random))!=1) throw std::runtime_error("Random generator failed");
        std::wstring name=L"ReDeskHost-";
        for(auto b:random) { name+=L"0123456789abcdef"[b>>4]; name+=L"0123456789abcdef"[b&15]; }
        auto directory=root/name;
        if (!CreateDirectoryW(directory.c_str(),nullptr))
            throw std::runtime_error("Cannot create isolated receive directory");
        validate_path(directory,true);
        return directory;
    }
    static bool safe_name(const std::string& name) {
        if (name.empty() || name.size()>240 || name.find_first_of("/\\:*?\"<>|")!=std::string::npos ||
            name.back()=='.' || name.back()==' ') return false;
        for(unsigned char c:name) if(c<32 || c==127) return false;
        const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),
                                     static_cast<int>(name.size()),nullptr,0);
        if(n<=0) return false;
        std::wstring wide(static_cast<std::size_t>(n),L'\0');
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),
                            static_cast<int>(name.size()),wide.data(),n)!=n) return false;
        // Reject DOS device names even when a file extension is appended.
        for(auto& c:wide) if(c>=L'a'&&c<=L'z') c-=L'a'-L'A';
        if(wide==L".PARTIAL" || wide.rfind(L".CROSSDESK-",0)==0) return false;
        auto stem=wide.substr(0,wide.find(L'.'));
        while(!stem.empty() && stem.back()==L' ') stem.pop_back();
        if(stem==L"CON"||stem==L"PRN"||stem==L"AUX"||stem==L"NUL"||
           stem==L"CONIN$"||stem==L"CONOUT$") return false;
        if(stem.size()==4 && (stem.rfind(L"COM",0)==0||stem.rfind(L"LPT",0)==0) &&
           ((stem[3]>=L'0'&&stem[3]<=L'9')||stem[3]==L'\u00b9'||stem[3]==L'\u00b2'||stem[3]==L'\u00b3'))
            return false;
        return true;
    }
    bool receive(const std::string& data) {
        using namespace crossdesk;
        FileChunkView chunk;
        if(!DecodeFileChunk(data.data(),data.size(),&chunk)) return false;
        const auto& h=chunk.header;
        if(!h.file_id || h.total_size>kMaxFile ||
           data.size()!=sizeof(FileChunkHeader)+h.name_len+chunk.payload_size ||
           chunk.payload_size>kFileChunkSize) return false;
        const bool cancel=h.flags==kFileCancelFlag;
        const bool first=(h.flags&1)!=0;
        if(cancel) {
            if(h.offset||h.name_len||h.chunk_size) return false;
        } else if((h.flags&~3u) || (first && (h.offset || !safe_name(chunk.file_name))) ||
                  (!first && h.name_len) || (!chunk.payload_size && !(h.flags&2)) ||
                  (((h.flags&2)!=0)!=(h.offset+chunk.payload_size==h.total_size))) return false;
        auto it=seen_.find(h.file_id);
        if(it==seen_.end()) {
            if(seen_.size()>=128 || h.total_size>kMaxTotal-reserved_) return false;
            reserved_+=h.total_size;
            it=seen_.emplace(h.file_id,Seen{h.total_size,false}).first;
        }
        if(it->second.total!=h.total_size) return false;
        if(it->second.done) return true; // don't recreate a finished/cancelled file
        session_.Receive(data.data(),data.size());
        if(cancel) it->second.done=true;
        // Shared engine reports protocol/write failures and cleans that file.
        return true;
    }
    SignalPeer& peer_;
    fs::path directory_;
    std::map<std::uint32_t,Seen> seen_;
    std::uint64_t reserved_=0;
    std::size_t completed_=0;
    bool failed_=false;
    crossdesk::file_transfer::Session session_;
};
}
