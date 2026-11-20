-- ReDesk-local Windows usrsctp recipe for reproducible cold builds.
-- Based on xmake-io/xmake-repo packages/u/usrsctp/xmake.lua.
-- Upstream recipe copyright: The Xmake Open Source Community, Apache-2.0.
-- Upstream usrsctp library: BSD-3-Clause.
-- This recipe is registered on Windows only by the root xmake.lua.
package("usrsctp")
    set_homepage("https://github.com/sctplab/usrsctp")
    set_description("A portable SCTP userland stack")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/sctplab/usrsctp/archive/refs/tags/$(version).tar.gz", {version = function (version)
        return version:gsub("%+", ".")
    end})
    add_urls("https://github.com/sctplab/usrsctp.git")
    add_versions("0.9.5+0", "260107caf318650a57a8caa593550e39bca6943e93f970c80d6c17e59d62cd92")

    -- Do not fetch upstream e984d7f3c or b56b4300b patches on Windows.
    -- Both alter Apple SDK / iOS code paths, not the Windows usrsctp build.
    -- Their archived GitHub .diff URLs caused a checksum mismatch in the
    -- tested toolchain. Keep the source version fixed instead of turning off
    -- checksum verification.

    add_configs("invariants", {description = "Add runtime checks", default = false, type = "boolean"})
    add_configs("inet", {description = "Support IPv4", default = true, type = "boolean"})
    add_configs("inet6", {description = "Support IPv6", default = true, type = "boolean"})
    add_configs("sanitizer_memory", {description = "Compile with memory sanitizer", default = false, type = "boolean"})

    add_deps("cmake")
    add_syslinks("ws2_32", "iphlpapi")

    on_install("!wasm", function (package)
        local configs = {"-Dsctp_werror=0", "-Dsctp_build_programs=0"}
        if package:is_debug() then
            table.insert(configs, "-DCMAKE_BUILD_TYPE=Debug")
            table.insert(configs, "-Dsctp_debug=1")
        else
            table.insert(configs, "-DCMAKE_BUILD_TYPE=Release")
            table.insert(configs, "-Dsctp_debug=0")
        end

        table.insert(configs, "-Dsctp_build_shared_lib=" .. (package:config("shared") and "1" or "0"))
        table.insert(configs, "-Dsctp_sanitizer_address=" .. (package:config("asan") and "1" or "0"))
        for name, enabled in pairs(package:configs()) do
            if not package:extraconf("configs", name, "builtin") then
                table.insert(configs, "-Dsctp_" .. name .. "=" .. (enabled and "1" or "0"))
            end
        end
        if package:is_plat("windows") and package:config("shared") then
            table.insert(configs, "-DCMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=1")
        end
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("usrsctp_init", {includes = "usrsctp.h"}))
    end)
