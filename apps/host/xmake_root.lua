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
