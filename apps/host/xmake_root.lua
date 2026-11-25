-- Included from the repository root; do not pull in Slint or SDL3.
local root = path.absolute("../..", os.scriptdir())
target("redesk-host-auth-test")
    set_kind("binary")
    add_packages("openssl3", "nlohmann_json")
    add_includedirs(path.join(root, "apps/host/core"),
        path.join(root, "apps/desktop/src/gui"),
        path.join(root, "deps/submodules/minirtc/src/api"))
    add_files(path.join(root, "apps/host/tests/host_auth_test.cpp"))
target_end()

if has_config("REDESK_HOST_TRANSPORT_PROBE") then
    target("redesk-host-signal-probe")
        set_kind("binary")
        set_default(false)
        add_deps("minirtc")
        add_packages("openssl3", "nlohmann_json")
        add_includedirs(path.join(root, "apps/host/core"),
            path.join(root, "apps/host/transport"),
            path.join(root, "apps/desktop/src/gui"),
            path.join(root, "deps/submodules/minirtc/src/api"))
        add_files(path.join(root, "apps/host/transport/signal_peer.cpp"),
            path.join(root, "apps/host/tests/signal_probe.cpp"))
    target_end()
end

-- Use the existing native GDI backend and shared NV12 frame pool. No GUI,
-- virtual display, privacy helper or WGC plugin is linked into this probe.
if has_config("REDESK_HOST_TRANSPORT_PROBE") then
    target("redesk-host-capture-probe")
        set_kind("binary")
        set_default(false)
        add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
        add_packages("spdlog", "libyuv")
        add_includedirs(path.join(root, "apps/host/platform/windows"),
            path.join(root, "apps/desktop/src/common"),
            path.join(root, "apps/desktop/src/log"),
            path.join(root, "apps/desktop/src/screen_capturer"),
            path.join(root, "apps/desktop/src/platform/windows/screen_capturer"),
            path.join(root, "libs/wire/include"),
            path.join(root, "deps/submodules/minirtc/src/api"))
        add_files(path.join(root, "apps/host/platform/windows/capture_source.cpp"),
            path.join(root, "apps/host/tests/capture_probe.cpp"),
            path.join(root, "apps/desktop/src/log/rd_log.cpp"),
            path.join(root, "apps/desktop/src/screen_capturer/captured_nv12_frame.cpp"),
            path.join(root, "apps/desktop/src/platform/windows/screen_capturer/screen_capturer_gdi.cpp"))
        add_syslinks("User32", "Gdi32")
    target_end()
end
