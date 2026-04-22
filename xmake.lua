set_project("windrose-query")
set_version("1.0.0")
set_languages("c++17")

add_rules("mode.release")

target("version")
    set_kind("shared")
    add_files("src/version.cpp", "src/a2s_server.cpp", "src/windrose_engine.cpp", "src/pattern_finder.cpp")
    add_headerfiles("src/config.h", "src/a2s_server.h", "src/windrose_engine.h", "src/pattern_finder.h")
    add_includedirs("src")
    set_filename("version.dll")
    set_targetdir("dist")

    if is_plat("windows") then
        add_syslinks("kernel32", "user32", "ws2_32")
    end
