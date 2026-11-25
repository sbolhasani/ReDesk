#pragma once
#include "signal_peer.h"
#include "interactive_desktop.h"
#include <file_transfer_core.h>
#include <windows.h>
#include <openssl/rand.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <limits>
#include <chrono>
#include <stdexcept>
#include <string_view>

namespace redesk::host::windows {
namespace fs = std::filesystem;

// Deliberately limited legacy file-transfer probe. No remote filesystem
// listing, arbitrary remote destination, execution, or directory RPC.
class FileProbe {
public:
    // The wire protocol uses 64-bit offsets. Only the destination's
    // available space, active-transfer count and bounded metadata limit
    // admission; there is no arbitrary per-file or per-session byte cap.
    static constexpr std::uint64_t kMaxFile=static_cast<std::uint64_t>(INT64_MAX);
    static constexpr std::uint64_t kFreeSpaceReserve=64ull*1024*1024;
    static constexpr std::size_t kMaxActive=4;
    static constexpr std::size_t kMaxTracked=4096;
    explicit FileProbe(SignalPeer& peer,const fs::path& root,const fs::path& outgoing)
        : peer_(peer),directory_(make_directory(root)),
          session_([this](const char* stream,const char* data,std::size_t size) {
                return active() ? peer_.send_file_data(stream,data,size) : -1;
            },[this](const std::string&,double value,bool sending,const std::string&) {
                if (sending && (value==1 || value<0))
                    std::cout << "File send: " << (value==1?"completed":"failed or cancelled") << std::endl;
            },{false,false,4},[this](const crossdesk::file_transfer::ReceiveState& state) {
                using crossdesk::file_transfer::ReceiveStatus;
                if (auto it=seen_.find(state.id);it!=seen_.end()) {
                    auto& file=it->second;
                    if (state.status==ReceiveStatus::Receiving) {
                        file.received=state.received;
                        return;
                    }
                    finish(file,state.status==ReceiveStatus::Completed);
                }
                if (state.status==ReceiveStatus::Completed) {
                    ++completed_;
                    std::cout << "File receive completed: id=" << state.id << ", " << state.received << " bytes" << std::endl;
                } else std::cout << "File receive failed or cancelled: id=" << state.id
                                 << ", " << state.received << "/" << state.total << " bytes" << std::endl;
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
        if (!active()) {
            if (peer_.status().file_queue_failed)
                std::cout << "File transfer stopped: receive queue overflow" << std::endl;
            stop(); return;
        }
        try {
            // Recheck the isolated receive directory before processing data.
            validate_path(directory_,true);
            for (const auto& packet:peer_.take_files()) {
                if (!active()) { stop(); return; }
                if (packet.stream==crossdesk::kFileFeedbackStream) {
                    session_.Ack(packet.payload.data(),packet.payload.size());
                    continue;
                }
                receive(packet.payload);
            }
            // Bound disk and transport work per iteration; let video/input run.
            for (unsigned i=0;i<4 && active();++i)
                if (!session_.Pump()) break;
            const auto now=std::chrono::steady_clock::now();
            if (now>=next_report_) {
                const auto outgoing=session_.Snapshot();
                if (outgoing.status==crossdesk::file_transfer::Status::Sending)
                    std::cout << "File send progress: " << outgoing.acknowledged << "/" << outgoing.total << " bytes (ACK)" << std::endl;
                for (const auto& entry:seen_)
                    if (!entry.second.done)
                        std::cout << "File receive progress: id=" << entry.first << ", "
                                  << entry.second.received << "/" << entry.second.total << " bytes" << std::endl;
                next_report_=now+std::chrono::seconds(5);
            }
        } catch (...) {
            std::cout << "File transfer stopped: local filesystem or protocol error" << std::endl;
            stop();
        }
    }
    std::size_t completed() const { return completed_; }
private:
    struct Seen {
        std::uint64_t total;
        std::uint64_t received=0;
        bool done=false;
        bool completed=false;
    };
    void finish(Seen& file,bool completed) {
        if (file.done) return;
        reserved_-=file.total;
        --active_count_;
        file.done=true;
        file.completed=completed;
    }
    void ack(std::uint32_t id,std::uint64_t total,bool completed) {
        const auto packet=crossdesk::EncodeFileTransferAck(
            {crossdesk::kFileAckMagic,id,completed?total:0,total,completed?1u:2u});
        if (active() && peer_.send_file_data(crossdesk::kFileFeedbackStream,packet.data(),packet.size())!=0)
            std::cout << "File feedback submission failed: id=" << id << std::endl;
    }
    void reject(const crossdesk::FileChunkHeader& h,const char* reason) {
        const auto it=seen_.find(h.file_id);
        if (it!=seen_.end()) {
            auto& file=it->second;
            if (file.done) { ack(h.file_id,h.total_size,false); return; }
            // Cancel the engine's staging file, never other transfers.
            session_.CancelReceive(h.file_id);
            finish(file,false); // idempotent if callback already finished it
        } else if (seen_.size()<kMaxTracked) {
            seen_.emplace(h.file_id,Seen{h.total_size,0,true,false});
        }
        std::cout << "File rejected: id=" << h.file_id << ", bytes=" << h.total_size
                  << ", reason=" << reason << std::endl;
        ack(h.file_id,h.total_size,false);
    }
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
    void receive(const std::string& data) {
        using namespace crossdesk;
        FileChunkView chunk;
        if(!DecodeFileChunk(data.data(),data.size(),&chunk)) {
            std::cout << "Malformed file packet ignored" << std::endl;
            return;
        }
        const auto& h=chunk.header;
        if(!h.file_id || h.total_size>kMaxFile ||
           data.size()!=sizeof(FileChunkHeader)+h.name_len+chunk.payload_size ||
           chunk.payload_size>kFileChunkSize) {
            reject(h,"invalid file header"); return;
        }
        const bool cancel=h.flags==kFileCancelFlag;
        const bool first=(h.flags&1)!=0;
        if(cancel) {
            if(h.offset||h.name_len||h.chunk_size) {
                reject(h,"invalid cancellation"); return;
            }
        } else if((h.flags&~3u) || (first && (h.offset || !safe_name(chunk.file_name))) ||
                  (!first && h.name_len) || (!chunk.payload_size && !(h.flags&2)) ||
                  (((h.flags&2)!=0)!=(h.offset+chunk.payload_size==h.total_size))) {
            reject(h,"invalid filename or chunk"); return;
        }
        auto it=seen_.find(h.file_id);
        if(it!=seen_.end()) {
            if(it->second.total!=h.total_size) { reject(h,"inconsistent file size"); return; }
            if(it->second.done) {
                ack(h.file_id,h.total_size,it->second.completed && !cancel);
                return;
            }
        } else {
            if(cancel) {
                if(seen_.size()<kMaxTracked)
                    seen_.emplace(h.file_id,Seen{h.total_size,0,true,false});
                ack(h.file_id,h.total_size,false);
                return;
            }
            if(seen_.size()>=kMaxTracked || active_count_>=kMaxActive) {
                reject(h,"too many transfers"); return;
            }
            std::error_code ec;
            const auto space=fs::space(directory_,ec);
            // Conservatively reserve each active file's full size, so a
            // future chunk cannot commit more storage than initially allowed.
            if(ec || space.available<kFreeSpaceReserve ||
               reserved_>space.available-kFreeSpaceReserve ||
               h.total_size>space.available-kFreeSpaceReserve-reserved_) {
                reject(h,"insufficient free disk space"); return;
            }
            it=seen_.emplace(h.file_id,Seen{h.total_size}).first;
            reserved_+=h.total_size;
            ++active_count_;
            std::cout << "File receive started: id=" << h.file_id << ", bytes=" << h.total_size << std::endl;
        }
        const bool accepted=session_.Receive(data.data(),data.size());
        if(cancel || !accepted) finish(it->second,false);
        // The shared engine sends its own ACK and clears failed staging files.
    }
    SignalPeer& peer_;
    fs::path directory_;
    std::map<std::uint32_t,Seen> seen_;
    std::uint64_t reserved_=0;
    std::size_t active_count_=0;
    std::chrono::steady_clock::time_point next_report_{};
    std::size_t completed_=0;
    bool failed_=false;
    crossdesk::file_transfer::Session session_;
};
}
