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
