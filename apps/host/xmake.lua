-- Foundation tests only; do not expose an incomplete remote host executable.
-- Keep this entry point independent of the desktop GUI dependency graph.
set_project("redesk-host-foundation")
set_xmakever("3.1.1")
set_languages("c++17")
add_rules("mode.release", "mode.debug")
target("redesk-host-model-test")
    set_kind("binary")
    add_includedirs("core")
    add_files("tests/host_model_test.cpp")
target_end()

target("redesk-host-runtime-test")
    set_kind("binary")
    add_includedirs("core")
    add_files("tests/server_runtime_test.cpp")
target_end()

target("redesk-host-settings-test")
    set_kind("binary")
    add_includedirs("core")
    add_files("tests/host_settings_test.cpp")
target_end()

if is_plat("windows") then
    target("redesk-host-windows-settings-test")
        set_kind("binary")
        set_runtimes("MT")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "UNICODE", "_UNICODE")
        add_includedirs("core", "platform/windows")
        add_files("tests/windows_settings_test.cpp",
                  "platform/windows/settings_protection.cpp",
                  "platform/windows/settings_store.cpp")
        add_syslinks("Crypt32", "Advapi32", "Bcrypt", "Shell32", "Ole32", "Uuid")
    target_end()
end
