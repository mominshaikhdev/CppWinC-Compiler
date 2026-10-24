// cppwinc_driver.ixx — build logic, run()
// Split from cppwinc.ixx for readability.
// This file is #included from cppwinc.ixx, not compiled independently.

// driver

void replace_all(std::string& value, std::string_view from, std::string_view to)
{
    std::size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos)
    {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// True system core count. GetSystemInfo ignores the CPU-affinity mask that
// std::thread::hardware_concurrency() respects, so a process accidentally
// pinned to one core still uses all the machine's cores ("my PC has 4 cores").
std::size_t system_core_count()
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    std::size_t n = si.dwNumberOfProcessors;
    if (n == 0) n = std::thread::hardware_concurrency();
    if (n < 2) n = 2;
    return n;
}

// locate the MSVC toolchain directory (modules\std.ixx lives under it):
// use the dev-prompt env var when present, otherwise vswhere
std::filesystem::path locate_msvc_toolchain()
{
    // honor the dev-prompt env var only when it points at a real
    // toolchain; otherwise fall through to vswhere for the latest install
    if (const char* tools = std::getenv("VCToolsInstallDir"))
    {
        std::filesystem::path candidate(tools);
        if (std::filesystem::exists(candidate / "bin" / "Hostx64" / "x64" / "cl.exe"))
        {
            return candidate;
        }
    }
    // vswhere ships with the Visual Studio Installer; both ProgramFiles
    // variables come from the environment, so no drive is hardcoded
    std::string vswhere;
    if (const char* pf = std::getenv("ProgramFiles(x86)"))
    {
        vswhere = std::string(pf) + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    }
    if (vswhere.empty())
    {
        if (const char* pf = std::getenv("ProgramFiles"))
        {
            vswhere = std::string(pf) + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
        }
    }
    // Candidate VS roots (filesystem fallback). Built from the environment
    // — never a hardcoded drive — covering both stable and pre-release
    // layouts: <root>\Microsoft Visual Studio\<year|major>\<edition>.
    auto candidate_vs_roots = []()
    {
        std::vector<std::filesystem::path> roots;
        auto push = [&](std::filesystem::path p)
        {
            if (!p.empty() && std::find(roots.begin(), roots.end(), p) == roots.end())
                roots.push_back(std::move(p));
        };
        if (const char* pf = std::getenv("ProgramFiles"))
            push(std::filesystem::path(pf) / "Microsoft Visual Studio");
        if (const char* pf = std::getenv("ProgramFiles(x86)"))
            push(std::filesystem::path(pf) / "Microsoft Visual Studio");
        if (roots.empty())
        {
            if (const char* sd = std::getenv("SystemDrive"))
            {
                push(std::filesystem::path(sd) / "\\Program Files\\Microsoft Visual Studio");
                push(std::filesystem::path(sd) / "\\Program Files (x86)\\Microsoft Visual Studio");
            }
        }
        return roots;
    };

    // newest usable toolset under a <vs>\VC\Tools\MSVC root, compared
    // numerically per dot-separated segment so "14.10" sorts above "14.9"
    // (plain lexicographic order gets that wrong).
    auto newest_toolset_under = [](std::filesystem::path const& tools_root,
                                   std::array<unsigned long, 4>& out_ver) -> std::filesystem::path
    {
        std::error_code ec;
        std::filesystem::path newest;
        std::array<unsigned long, 4> newest_ver{};
        auto parse_ver = [](std::string const& s, std::array<unsigned long, 4>& v)
        {
            v = { 0, 0, 0, 0 };
            int idx = 0;
            unsigned long cur = 0;
            bool any = false;
            for (char c : s)
            {
                if (c >= '0' && c <= '9') { cur = cur * 10 + static_cast<unsigned long>(c - '0'); any = true; }
                else { if (any && idx < 4) v[idx++] = cur; cur = 0; any = false; }
            }
            if (any && idx < 4) v[idx] = cur;
        };
        if (!std::filesystem::is_directory(tools_root, ec)) return {};
        for (auto const& entry : std::filesystem::directory_iterator(tools_root, ec))
        {
            if (!entry.is_directory()) continue;
            // A toolset entry is only usable when the x64-hosted cross
            // compiler exists (ARM64-host-only or broken installs are
            // skipped so the scan can fall through to an older but
            // complete toolset).
            if (!std::filesystem::exists(entry.path() / "bin" / "Hostx64" / "x64" / "cl.exe")) continue;
            std::array<unsigned long, 4> ver{};
            parse_ver(entry.path().filename().string(), ver);
            if (newest.empty() || ver > newest_ver)
            {
                newest = entry.path();
                newest_ver = ver;
            }
        }
        out_ver = newest_ver;
        return newest;
    };

    std::array<unsigned long, 4> best_ver{};
    std::filesystem::path best_toolset;

    if (!vswhere.empty() && std::filesystem::exists(vswhere))
    {
        // -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
        // -prerelease is required so pre-release channel installs are
        // discovered too; stable-only machines are unaffected because
        // -prerelease merely *permits* prereleases.
        // If the requirement filter yields nothing (component id drift or
        // an instance flagged isComplete=0), retry without it, then fall
        // through to the filesystem scan.
        std::string const require_clause = " -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64";
        for (int attempt = 0; attempt < 2 && best_toolset.empty(); ++attempt)
        {
            std::string query = "\"" + vswhere + "\" -latest -products * -prerelease";
            query += (attempt == 0 ? require_clause : "");
            query += " -property installationPath";
            FILE* pipe = _popen(query.c_str(), "r");
            if (!pipe)
            {
                break;
            }
            char buffer[1024];
            std::string result;
            while (fgets(buffer, sizeof(buffer), pipe))
            {
                result += buffer;
            }
            _pclose(pipe);
            // keep only the first line (vswhere can emit multiple installs)
            std::size_t nl = result.find_first_of("\r\n");
            if (nl != std::string::npos) result.resize(nl);
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            {
                result.pop_back();
            }
            if (result.empty() || !std::filesystem::exists(result))
            {
                continue;
            }
            std::array<unsigned long, 4> ver{};
            auto toolset = newest_toolset_under(std::filesystem::path(result) / "VC" / "Tools" / "MSVC", ver);
            if (!toolset.empty())
            {
                best_toolset = toolset;
                best_ver = ver;
            }
        }
    }

    // Filesystem fallback: scan every candidate VS root for usable MSVC
    // toolsets. This rescues installs vswhere cannot see (isComplete=0
    // after an interrupted update, Build Tools without the installer
    // registration, portable layouts) — exactly the machines where the
    // toolchain is present but the metadata lookup lies.
    {
        std::error_code sec;
        for (auto const& vs_root : candidate_vs_roots())
        {
            if (!std::filesystem::is_directory(vs_root, sec)) continue;
            // VS layouts differ across releases ("<root>\\<year|major>\\<edition>"),
            // so walk two directory levels and probe each folder for the
            // standard "VC\\Tools\\MSVC" toolset subtree.
            for (auto const& lvl1 : std::filesystem::directory_iterator(vs_root, sec))
            {
                std::error_code e1;
                if (!lvl1.is_directory(e1) || e1) continue;
                for (auto const& lvl2 : std::filesystem::directory_iterator(lvl1.path(), e1))
                {
                    std::error_code e2;
                    if (!lvl2.is_directory(e2) || e2) continue;
                    std::array<unsigned long, 4> ver{};
                    auto toolset = newest_toolset_under(lvl2.path() / "VC" / "Tools" / "MSVC", ver);
                    if (!toolset.empty() && (best_toolset.empty() || ver > best_ver))
                    {
                        best_toolset = toolset;
                        best_ver = ver;
                    }
                }
            }
        }
    }

    return best_toolset;
}

// build the std named module (std.ixx) into the given folder so
// `import std;` works from any terminal — the command-line equivalent of
// MSBuild's <BuildStlModules>true</BuildStlModules>. Prints the
// /ifcSearchDir to use. Returns false on failure.
bool build_std_module(std::filesystem::path const& out_dir)
{
    std::filesystem::path toolchain = locate_msvc_toolchain();
    if (toolchain.empty())
    {
        std::printf("cppwinc : error could not locate the MSVC toolchain (set VCToolsInstallDir or install the VC Tools workload)\n");
        return false;
    }
    std::filesystem::path std_ixx = toolchain / "modules" / "std.ixx";
    if (!std::filesystem::exists(std_ixx))
    {
        std::printf("cppwinc : error std module source not found: %s\n", std_ixx.string().c_str());
        return false;
    }
    std::filesystem::create_directories(out_dir);
    std::error_code ec;
    std::string abs = std::filesystem::weakly_canonical(out_dir, ec).string();
    std::string toolchain_label = toolchain.filename().string();
    if (toolchain_label.empty()) toolchain_label = toolchain.parent_path().filename().string();
    std::printf("cppwinc : building std module (%s) into %s\n", toolchain_label.c_str(), abs.c_str());

    std::string cl = (toolchain / "bin" / "Hostx64" / "x64" / "cl.exe").string();
    std::string q = "\"";
    // cl needs the toolchain + Windows SDK include dirs on the command
    // line; outside a dev prompt none of them are set. Locate the newest
    // installed SDK version.
    std::string include = (toolchain / "include").string();
    // locate the Windows SDK through the environment (never a hardcoded
    // drive): newest version under %ProgramFiles(x86)%\Windows Kits\10\Include
    std::string sdk_inc;
    {
        std::filesystem::path kits;
        if (const char* pf_x86 = std::getenv("ProgramFiles(x86)"))
        {
            kits = std::filesystem::path(pf_x86) / "Windows Kits" / "10" / "Include";
        }
        else if (const char* pf = std::getenv("ProgramFiles"))
        {
            kits = std::filesystem::path(pf) / "Windows Kits" / "10" / "Include";
        }
        std::error_code sec;
        std::filesystem::path newest;
        if (!kits.empty())
        {
            for (auto const& entry : std::filesystem::directory_iterator(kits, sec))
            {
                auto fname = entry.path().filename().string();
                // Version dirs only (10.0.x.x): the Kits\10\Include root also
                // carries non-version folders like 'wdf' that outrank versions
                // in a string compare and break the ucrt include path.
                if (entry.is_directory() && !fname.empty() &&
                    std::isdigit(static_cast<unsigned char>(fname.front())) &&
                    (newest.empty() || fname > newest.filename().string()))
                {
                    newest = entry.path();
                }
            }
        }
        if (!newest.empty())
        {
            sdk_inc = newest.string();
        }
    }
    // cl needs the toolchain + SDK include dirs; outside a dev prompt
    // none are set, so set INCLUDE/LIB in the batch file (quotes around
    // /I paths with spaces trip up cmd's argument parsing)
    std::string include_env = "set \"INCLUDE=" + include + ";";
    if (!sdk_inc.empty())
    {
        include_env += sdk_inc + "\\ucrt;" + sdk_inc + "\\shared;" + sdk_inc + "\\um;" + sdk_inc + "\\winrt";
    }
    include_env += "\"\r\n";
    // cd into the output dir so cl drops std.obj there (a quoted /Fo
    // ending in a backslash trips up cmd's argument parsing)
    std::string cmd = "cd /d " + q + abs + q + "\n"
        + q + cl + q + " /nologo /std:c++latest /EHsc /MD /O2 /DNDEBUG /utf-8 /c "
        + q + std_ixx.string() + q
        + " /ifcOutput " + q + abs + q;
    // run through a temporary batch file: cmd /c strips quotes from an
    // inline command, and a batch file preserves them exactly
    std::filesystem::path bat = std::filesystem::temp_directory_path() / "cppwinc_std.bat";
    {
        std::ofstream out(bat);
        out << "@echo off\n" << include_env << cmd << "\n";
    }
    std::string run = q + bat.string() + q;
    int rc = std::system(run.c_str());
    {
        std::error_code unlink_ec;
        std::filesystem::remove(bat, unlink_ec);
    }
    if (rc != 0)
    {
        std::printf("cppwinc : error cl failed to build the std module (%d)\n", rc);
        return false;
    }
    std::printf("cppwinc : std module built.\n");
    std::printf("  use: /ifcSearchDir \"%s\"   (and link std.obj from that folder)\n", abs.c_str());
    return true;
}

// "<product> v<version> - <descriptor>" — mirrors winversec's banner so both
// compilers announce themselves the same way.
void print_version_banner()
{
    std::printf("%s v%s - %s\n",
        std::string(cppwinc::strings::product_name).c_str(),
        std::string(cppwinc::strings::version_string).c_str(),
        std::string(cppwinc::strings::product_description).c_str());
}

void print_usage()
{
    print_version_banner();
    std::printf("%s\n", std::string(cppwinc::strings::copyright).c_str());
    std::printf("\n");
    std::printf("  cppwinc.exe [options...]\n\n");
    std::printf("Options:\n");
    for (auto const& opt : options)
    {
        if (opt.desc.empty())
        {
            continue;
        }
        std::string name = "  -";
        name += opt.name;
        if (!opt.arg.empty())
        {
            name += " ";
            name += opt.arg;
        }
        std::printf("  %-22s %s\n", name.c_str(), std::string(opt.desc).c_str());
    }
    std::printf("\n-input <spec> is ONE comma-separated list of any of:\n");
    std::printf("  path                Path to winmd file or recursively scanned folder\n");
    std::printf("  local               Local %s\\System32\\WinMetadata folder\n", "%WinDir%");
    std::printf("  10.0.12345.0        Windows SDK metadata for a specific version\n");
    std::printf("\n  -winui / -winui_no_compile auto-discover the WASDK metadata (NuGet cache)\n");
    std::printf("  and auto-locate the WASDK runtime DLLs - no wasdk input needed.\n");
    std::printf("e.g. -input local,MyApp.winmd -winui_no_compile\n");
}

void process_args(arg_parser const& args)
{
    settings.verbose = args.exists("verbose");
    settings.component = args.exists("component");
    settings.base = args.exists("base");

    // `-winui`: auto-discover WASDK metadata + detect runtime DLLs.
    // Like `-std` auto-builds the std module, `-winui` auto-adds all
    // WASDK inputs and reports DLL availability for link-time.
    if (args.exists("winui") && args.exists("winui_no_compile"))
    {
        throw winmd::error("-winui and -winui_no_compile are mutually exclusive");
    }
    bool has_winui = args.exists("winui") || args.exists("winui_no_compile");
    settings.winui = has_winui;
    settings.winui_no_compile = args.exists("winui_no_compile");
    settings.compile = args.exists("compile");
    if (settings.compile && has_winui)
    {
        throw winmd::error("-compile and -winui are mutually exclusive (-winui already compiles)");
    }
    if (has_winui)
    {
        settings.base = true;  // auto-generate winrt_base.ixx
        settings.component = true;  // auto-generate component modules
        auto nuget = nuget_packages_root();
        if (!nuget.empty())
        {
            // -winui always auto-discovers WASDK metadata (NuGet cache) —
            // there is no explicit wasdk input spec.
            {
                auto wasdk_paths = resolve_wasdk();
                if (wasdk_paths.empty())
                {
                    throw winmd::error("-winui: no Windows App SDK metadata found in the NuGet cache\n"
                        "  looked in: " + nuget_packages_root().string() + "\n"
                        "  fix: restore any WASDK-based project once (msbuild /t:restore or 'nuget restore'),\n"
                        "  or set NUGET_PACKAGES if your package cache lives elsewhere");
                }
                for (auto const& p : wasdk_paths)
                    settings.input.push_back(p);
                std::printf("-winui: auto-added %zu WASDK winmds\n", wasdk_paths.size());
            }
            // -winui also auto-adds the Windows SDK UnionMetadata so types the
            // WASDK metadata references (Windows.Foundation TypedEventHandler /
            // IReference, Windows.UI.Text.FontWeight / FontStyle, ...) resolve
            // instead of falling back to IInspectable / failing to compile.
            {
                std::filesystem::path um_root;
                if (const char* pf_x86 = std::getenv("ProgramFiles(x86)"))
                    um_root = std::filesystem::path(pf_x86) / "Windows Kits" / "10" / "UnionMetadata";
                else if (const char* pf = std::getenv("ProgramFiles"))
                    um_root = std::filesystem::path(pf) / "Windows Kits" / "10" / "UnionMetadata";
                std::error_code umec;
                std::filesystem::path newest_um;
                if (!um_root.empty() && std::filesystem::is_directory(um_root, umec))
                {
                    for (auto const& entry : std::filesystem::directory_iterator(um_root, umec))
                    {
                        // only numeric version folders (10.0.xxxxx.0) — skip
                        // non-version dirs like "Facade"
                        if (!entry.is_directory()) continue;
                        auto fn = entry.path().filename().string();
                        if (fn.empty() || !std::isdigit(static_cast<unsigned char>(fn.front()))) continue;
                        if (newest_um.empty() || fn > newest_um.filename().string())
                            newest_um = entry.path();
                    }
                }
                std::size_t added = 0;
                if (!newest_um.empty())
                {
                    for (auto const& entry : std::filesystem::directory_iterator(newest_um, umec))
                    {
                        if (!entry.is_directory() && entry.path().extension() == ".winmd")
                        {
                            settings.input.push_back(entry.path());
                            ++added;
                        }
                    }
                }
                if (added == 0)
                {
                    // fall back to the OS WinMetadata folder
                    std::filesystem::path local_dir = std::filesystem::path(std::getenv("WinDir")) / "System32" / "WinMetadata";
                    if (std::filesystem::is_directory(local_dir))
                    {
                        for (auto const& entry : std::filesystem::directory_iterator(local_dir))
                        {
                            if (!entry.is_directory() && entry.path().extension() == ".winmd")
                            {
                                settings.input.push_back(entry.path());
                                ++added;
                            }
                        }
                    }
                }
                if (added > 0)
                    std::printf("-winui: auto-added %zu Windows SDK winmds (newest UnionMetadata)\n", added);
            }
            // Detect WinUI3 runtime DLLs in NuGet cache
            // Find the runtime DLLs across ALL installed versions, newest
            // first (lexicographic sort works for semver), probing every
            // architecture's runtimes-framework native folder.
            auto winui_pkg = nuget / L"microsoft.windowsappsdk.winui";
            if (std::filesystem::is_directory(winui_pkg))
            {
                std::vector<std::filesystem::path> versions;
                for (auto const& entry : std::filesystem::directory_iterator(winui_pkg))
                {
                    if (!entry.is_directory()) continue;
                    auto p = entry.path().filename().string();
                    if (!p.empty() && std::isdigit(static_cast<unsigned char>(p.front())))
                        versions.push_back(entry.path());
                }
                std::sort(versions.begin(), versions.end(), [](auto const& a, auto const& b)
                {
                    return a.filename().string() > b.filename().string();
                });
                static const wchar_t* arches[] = { L"win-x64", L"win-arm64", L"win-x86" };
                for (auto const& ver : versions)
                {
                    if (!settings.winui_dll_path.empty()) break;
                    for (auto const* arch : arches)
                    {
                        auto native = ver / L"runtimes-framework" / arch / L"native";
                        if (!std::filesystem::is_directory(native)) continue;
                        auto dll = native / L"Microsoft.ui.xaml.dll";
                        if (std::filesystem::exists(dll))
                        {
                            settings.winui_dll_path = native.string();
                            std::printf("-winui: runtime DLLs found at %s\n", settings.winui_dll_path.c_str());
                            break;
                        }
                    }
                }
            }
            if (settings.winui_dll_path.empty())
            {
                // Check system framework package. %ProgramFiles% comes from
                // the environment (never a hardcoded drive letter), so this
                // works on machines where Windows is not on C:.
                std::error_code ec;
                std::filesystem::path windows_apps;
                if (const char* pf_env = std::getenv("ProgramFiles"))
                    windows_apps = std::filesystem::path(pf_env) / "WindowsApps";
                else if (const wchar_t* sys_drive = _wgetenv(L"SystemDrive"))
                    windows_apps = std::filesystem::path(sys_drive) / L"\\Program Files\\WindowsApps";
                if (std::filesystem::is_directory(windows_apps, ec))
                for (auto const& entry : std::filesystem::directory_iterator(windows_apps, ec))
                {
                    if (!entry.is_directory()) continue;
                    auto name = entry.path().filename().string();
                    if (name.find("Microsoft.WindowsAppRuntime") != std::string::npos)
                    {
                        auto dll = entry.path() / L"Microsoft.ui.xaml.dll";
                        if (std::filesystem::exists(dll))
                        {
                            settings.winui_dll_path = entry.path().string();
                            std::printf("-winui: system framework package found at %s\n", settings.winui_dll_path.c_str());
                            break;
                        }
                    }
                }
            }
            if (settings.winui_dll_path.empty())
                std::printf("-winui: runtime DLLs not found; building projection from metadata\n");
            }
            // -winui / -winui_no_compile: the bootstrap module (main.ixx)
            // imports winversec-projected Win32 modules (PACKAGE_VERSION,
            // TryCreatePackageDependency, MddBootstrap*, LoadLibraryExW, ...)
            // instead of including <windows.h>/<appmodel.h>/<MddBootstrap.h>.
            // The winversec winmd is NOT auto-located and has no dedicated
            // switch: it is provided manually as a regular comma-separated
            // -input entry, exactly like any other winmd. Without -winui it
            // is projected like any other input; with -winui its absence is
            // a hard error - a WinUI app's bootstrap cannot be generated
            // header-free without it.
            {
                bool have_win32_md = false;
                for (auto const& spec : args.values("input"))
                {
                    std::string low = spec;
                    for (auto& ch : low)
                        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
                    if (low.find("windows.win32") != std::string::npos)
                    {
                        have_win32_md = true;
                        break;
                    }
                }
                if (!have_win32_md)
                {
                    throw winmd::error("-winui: no winversec-produced Windows.Win32.winmd in -input\n"
                        "  cppwinc projects the bootstrap's Win32 APIs (PACKAGE_VERSION, TryCreatePackageDependency,\n"
                        "  MddBootstrap*) from winversec metadata instead of including Windows headers.\n"
                        "  provide it manually as a regular comma-separated -input entry, e.g.:\n"
                        "    cppwinc -input local,winuic_out/App.winmd,out/Windows.Win32.winmd -winui -output out\n"
                        "  generate the winmd with:  winversec -include_wasdk -output Windows.Win32.winmd");
                }
            }
        }



    bool first_input = true;
    auto is_system_md = [](std::wstring const& p)
    {
        std::wstring low = p;
        for (auto& ch : low)
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        return low.find(L"system32\\winmetadata") != std::wstring::npos
            || low.find(L"windows kits\\10\\references") != std::wstring::npos;
    };
    for (auto const& spec : args.values("input"))
    {
        for (auto const& path : resolve_spec(spec))
        {
            settings.input.push_back(path);
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(path, ec).string();
            // Only the first USER (app) input .winmd is the component source.
            // System metadata - the 'local' spec (expands to every OS winmd,
            // alphabetically starting with Windows.AI.winmd) and SDK contract
            // winmds - must never name the component, or the app links as
            // Windows.AI.exe with a Windows.AI.pri instead of <App>.exe.
            if (first_input && spec != "local" && !is_system_md(path.wstring()))
            {
                settings.input_dbs.insert(canonical);
                first_input = false;
            }
        }
    }
    // Comma-separated lists accepted: a single flag value can carry several
    // namespaces (leading/trailing spaces trimmed).
    auto split_ns_list = [](std::string const& spec,
                            std::set<std::string, std::less<>>& sink)
    {
        std::string item;
        auto flush = [&]()
        {
            if (!item.empty())
            {
                sink.insert(item);
                item.clear();
            }
        };
        for (char c : spec)
        {
            if (c == ',') flush();
            else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') item.push_back(c);
        }
        flush();
    };
    for (auto const& spec : args.values("include"))
    {
        split_ns_list(spec, settings.include);
    }
    for (auto const& spec : args.values("exclude"))
    {
        split_ns_list(spec, settings.exclude);
    }
    for (auto const& spec : args.values("module_include"))
    {
        split_ns_list(spec, settings.module_include);
    }
    for (auto const& spec : args.values("module_exclude"))
    {
        split_ns_list(spec, settings.module_exclude);
    }

    std::string output = args.value("output", ".");
    std::filesystem::create_directories(output);
    settings.output_folder = std::filesystem::canonical(output).string();
    settings.output_folder += std::filesystem::path::preferred_separator;

    if (settings.component)
    {
        std::string component = args.value("component");
        if (!component.empty())
        {
            std::filesystem::create_directories(component);
            settings.component_folder = std::filesystem::canonical(component).string();
            settings.component_folder += std::filesystem::path::preferred_separator;
        }
        settings.component_name = args.value("name");
        if (settings.component_name.empty())
        {
            // Component source = the app .winmd(s): EVERY non-system .winmd
            // named in -input, in any order. System/projection winmds
            // (Windows.Win32, Windows.Wdk, Windows, Microsoft.*) are skipped,
            // so naming Windows.Win32 first can no longer produce a 84MB
            // component file that clobbered the SCC group module of the same
            // name and broke every import of it — and no position in -input
            // decides which winmd is "the app": winuic view winmds and
            // component winmds qualify alike.
            // args.values("input") returns the raw -input value(s), and the
            // spec is ONE comma-separated list. Split on commas before
            // scanning for the app winmds.
            std::vector<std::string> inputs;
            for (auto const& raw : args.values("input"))
            {
                std::string::size_type start = 0;
                for (;;)
                {
                    auto comma = raw.find(',', start);
                    if (comma == std::string::npos)
                    {
                        inputs.emplace_back(raw.substr(start));
                        break;
                    }
                    inputs.emplace_back(raw.substr(start, comma - start));
                    start = comma + 1;
                }
            }
            std::string best_stem;
            for (auto const& in : inputs)
            {
                if (in.size() > 6 && in.rfind(".winmd") == in.size() - 6)
                {
                    std::string stem = std::filesystem::path(in).filename().replace_extension().string();
                    if (stem == "Windows.Win32" || stem == "Windows.Wdk" || stem == "Windows" || stem.starts_with("Microsoft."))
                        continue;
                    std::error_code app_ec;
                    settings.app_dbs.insert(std::filesystem::weakly_canonical(in, app_ec).string());
                    // Default file name stays order-independent too: with a
                    // single app winmd (the normal case) it is its stem;
                    // with several, the lexicographic first wins.
                    if (best_stem.empty() || stem < best_stem)
                        best_stem = stem;
                }
            }
            if (!best_stem.empty())
            {
                settings.component_name = best_stem;
            }
            if (settings.component_name.empty() && !settings.input_dbs.empty())
            {
                settings.component_name = std::filesystem::path(*settings.input_dbs.begin()).filename().replace_extension().string();
            }
        }
    }
}

//
// Win32 API projection writers
//

// Win32 handle type mapping: metadata type names to projected void* wrappers
static std::string win32_handle_projected(std::string_view type_name)
{
    static const std::unordered_map<std::string_view, std::string_view> handle_map = {
        {"HANDLE", "void*"}, {"HWND", "void*"}, {"HDC", "void*"},
        {"HKEY", "void*"}, {"HMODULE", "void*"}, {"HINSTANCE", "void*"},
        {"HBITMAP", "void*"}, {"HBRUSH", "void*"}, {"HFONT", "void*"},
        {"HPEN", "void*"}, {"HRGN", "void*"}, {"HICON", "void*"},
        {"HCURSOR", "void*"}, {"HMENU", "void*"}, {"HACCEL", "void*"},
        {"HMONITOR", "void*"}, {"HGDIOBJ", "void*"}, {"HGLOBAL", "void*"},
        {"HLOCAL", "void*"}, {"HRSRC", "void*"}, {"HWINSTA", "void*"},
        {"HDESK", "void*"}, {"SC_HANDLE", "void*"}, {"SOCKET", "void*"},
        {"HINTERNET", "void*"}, {"HCRYPTPROV", "void*"}, {"HCRYPTKEY", "void*"},
        {"HCRYPTHASH", "void*"}, {"HCERTSTORE", "void*"}, {"HCRYPTMSG", "void*"},
        {"HDEVNOTIFY", "void*"}, {"HPOWERNOTIFY", "void*"}, {"HDROP", "void*"},
    };
    auto it = handle_map.find(type_name);
    return it != handle_map.end() ? std::string(it->second) : "";
}

// Enum member names that collide with C++ keywords or alternative-operator
// tokens (ICU `or = 201`, TEXTSERV `not = ...`) are a hard syntax error
// (C2143) if emitted verbatim — mangle with a trailing underscore.
static std::string mangle_enum_member(std::string const& n)
{
    static const std::set<std::string> kw = {
        "alignas","alignof","and","and_eq","asm","auto","bitand","bitor","bool","break",
        "case","catch","char","char8_t","char16_t","char32_t","class","compl","concept",
        "const","consteval","constexpr","constinit","const_cast","continue","co_await",
        "co_return","co_yield","decltype","default","delete","do","double","dynamic_cast",
        "else","enum","explicit","export","extern","false","float","for","friend","goto",
        "if","inline","int","long","mutable","namespace","new","noexcept","not","not_eq",
        "nullptr","operator","or","or_eq","private","protected","public","register",
        "reinterpret_cast","requires","return","short","signed","sizeof","static",
        "static_assert","static_cast","struct","switch","template","this","thread_local",
        "throw","true","try","typedef","typeid","typename","union","unsigned","using",
        "virtual","void","volatile","wchar_t","while","xor","xor_eq",
        "near","far","pascal","cdecl","stdcall","small"
    };
    if (kw.count(n)) return n + "_";
    return n;
}

static void write_win32_enum(writer& w, winmd::type_info const& type, type_mapper const& mapper)
{
    if (type.has_irp_major_function)
        w.write("// IRP major function code: %\n", type.irp_major_function);
    if (type.ioctl_code)
        w.write("// IOCTL code: % (buffering method %)\n", type.ioctl_code, type.ioctl_buffering);
    std::string underlying = enum_underlying_cpp(type.enum_underlying);
    if (underlying.empty()) underlying = "std::int32_t";
    // Singleton "const enums": winversec materializes every numeric #define
    // constant (GENERIC_READ, KEY_READ, WM_DESTROY, the predefined HKEY_*
    // roots, ...) as a one-member enum — the metadata-native constant shape.
    // Render those as plain exported constants; an enum class whose only
    // spelling would be GENERIC_READ::GENERIC_READ is unusable. IOCTL/IRP
    // singletons keep their enum form (carrying the decode comments).
    if (type.enum_fields.size() == 1 && type.enum_fields[0].first == type.name
        && !type.ioctl_code && !type.has_irp_major_function)
    {
        // GENERIC_READ (0x80000000) and the predefined HKEY_* roots
        // (0x80000001) don't fit the default int32 underlying — a literal
        // wider than its declared type is ill-formed (C2397). The
        // materializer tags every constant field U4, so widen when the
        // value needs it.
        std::string const_underst = underlying;
        if (type.enum_fields[0].second > 2147483647LL ||
            type.enum_fields[0].second < -2147483648LL)
            const_underst = "std::uint32_t";
        // #undef first: BUFSIZ/EXIT_SUCCESS/FILENAME_MAX/RAND_MAX are CRT
        // macros visible in this TU — without it the token expands to a
        // literal before the declaration parses (C2332/C2059). Macros never
        // cross a module boundary, so importers never see this name anyway.
        w.write("#undef %\n", mangle_enum_member(type.name));
        w.write("export inline constexpr % % = %;\n\n", const_underst,
            mangle_enum_member(type.name), type.enum_fields[0].second);
        return;
    }
    // Same macro guard as the singleton-const path above.
    w.write("#undef %\n", type.name);
    w.write("export enum class % : %\n{\n", type.name, underlying);
    // Keep the first definition of each enumerator name. Some SDK headers
    // define the same enumerator twice (e.g. MSI's SCRIPTFLAGS_REGDATA_APPINFO
    // = 0 then = 16); emitting both is a C2086 redefinition error.
    {
        std::set<std::string> seen_enum;
        for (auto const& [name, value] : type.enum_fields)
        {
            if (!seen_enum.insert(name).second) continue;
            w.write("    % = %,\n", mangle_enum_member(name), value);
        }
    }
    w.write("};\n\n");
    if (type.enum_flags)
    {
        // 6/6/4 placeholders: signature names + OUTER cast are the enum
        // type, the inner casts are the underlying type (5 args left a
        // literal `%` in every FLAGS enum: `static_cast<%>(b)` — C2760).
        // `export` so importing modules can actually use the operators.
        w.write("export constexpr % operator|(% a, % b) noexcept { return static_cast<%>(static_cast<%>(a) | static_cast<%>(b)); }\n",
            type.name, type.name, type.name, type.name, underlying, underlying);
        w.write("export constexpr % operator&(% a, % b) noexcept { return static_cast<%>(static_cast<%>(a) & static_cast<%>(b)); }\n",
            type.name, type.name, type.name, type.name, underlying, underlying);
        w.write("export constexpr % operator~(% a) noexcept { return static_cast<%>(~static_cast<%>(a)); }\n\n",
            type.name, type.name, type.name, underlying);
    }
}

// Emit Win32/WDK structs in value-field dependency order (Kahn's
// algorithm). Metadata row order does not guarantee definition-before-use
// inside a namespace (parallel-parse merge order), and a value member
// (`in_addr sin_addr;`, `LARGE_INTEGER Size;`) needs the complete type at
// that point — out-of-order emission broke the defining module itself and
// C2230-cascaded through every importer (WinSock -> NSPAPI -> NTDEF ->
// Com -> half the SDK). Cycles (mutual recursion by pointer is legal in
// C but value-members cannot cycle) are emitted in residual metadata
// order, matching the old behavior for the (impossible) cycle case.
template <typename EmitFn>
static void write_structs_topo(writer& w,
    std::vector<winmd::type_info const*> const& structs,
    winmd::metadata const& metadata, EmitFn&& emit)
{
    // Hoisted anonymous-union/struct helpers (`_AnonymousUnion`,
    // `_AnonymousUnion_3`, ...) are embedded BY VALUE in their enclosing
    // struct (NET_BUFFER::USHORT, NET_BUFFER_LIST::NET_BUFFER_LIST_CONTEXT),
    // but the reference does not always survive winmd field encoding, so
    // the dep pass below can miss it and emit the enclosing struct first
    // (C2079 '_AnonymousUnion' undefined in NBL -> NBL dies -> Windows dies
    // -> the whole SDK cascades). winversec hoists innermost-first, so the
    // helpers' relative order is already dependency-correct; hoist the
    // whole group ahead of every other struct. The helpers never embed a
    // non-helper struct by value (they are nested INSIDE the outer struct,
    // so such a reference would be recursive and illegal C), so this is
    // always safe.
    // Keep this pre-pass PREFIX-only and unsorted: infix-named hoists
    // (tagPROPVARIANT_AnonymousUnion) are ordered by the Kahn pass, whose
    // walker only follows by-value edges (no ptr/fnptr descent).
    std::vector<winmd::type_info const*> ordered;
    ordered.reserve(structs.size());
    for (auto const* t : structs)
        if (t->name.rfind("_Anonymous", 0) == 0) ordered.push_back(t);
    for (auto const* t : structs)
        if (t->name.rfind("_Anonymous", 0) != 0) ordered.push_back(t);

    std::map<winmd::type_info const*, std::set<winmd::type_info const*>> field_deps;
    std::set<winmd::type_info const*> remaining(ordered.begin(), ordered.end());
    for (auto const* t : structs)
    {
        for (auto const& f : t->fields)
        {
            std::vector<winmd::type const*> stack{ &f.value };
            while (!stack.empty())
            {
                auto const* ty = stack.back();
                stack.pop_back();
                if (ty->kind == winmd::et_class || ty->kind == winmd::et_valuetype || ty->kind == winmd::et_genericinst)
                {
                    // metadata.find misses named-type refs in signatures (see
                    // collect_ref_ns); fall back to the row's own ns/name so
                    // hoisted anonymous unions still order before the structs
                    // that embed them by value (NBL_NET_BUFFER_LIST).
                    winmd::type_info const* dep = metadata.find(*ty);
                    if (!dep)
                    {
                        std::string dns, dname;
                        if (metadata.resolve_type_name(*ty, dns, dname) && !dns.empty())
                            dep = metadata.find(dns, dname);
                    }
                    if (dep)
                    {
                        if (dep->is_struct() && remaining.contains(dep))
                        {
                            field_deps[t].insert(dep);
                        }
                    }
                }
                // By-value carriers only: ptr/byref/fnptr members do not
                // require complete targets, and walking them fabricates
                // false cycles (pElems -> tagPROPVARIANT) that stall Kahn.
                if (ty->kind != winmd::et_ptr && ty->kind != winmd::et_byref &&
                    ty->kind != winmd::et_fnptr)
                {
                    for (auto const& a : ty->args) stack.push_back(&a);
                    for (auto const& c : ty->children) stack.push_back(&c);
                }
            }
        }
    }
    std::set<winmd::type_info const*> done;
    while (done.size() < ordered.size())
    {
        bool progress = false;
        for (auto const* t : ordered)
        {
            if (done.contains(t)) continue;
            bool ready = true;
            for (auto const* d : field_deps[t])
            {
                if (!done.contains(d)) { ready = false; break; }
            }
            if (ready)
            {
                emit(w, *t);
                done.insert(t);
                progress = true;
            }
        }
        if (!progress) break;
    }
    for (auto const* t : ordered)
    {
        if (!done.contains(t)) emit(w, *t);
    }
}

// Names that must never become a `using X = ...;` alias. C++ keywords
// (`using wchar_t = void*;` is a hard C2059 syntax error — the keyword is
// natively available to consumers anyway) and SAL annotation macro names
// that winversec parse debris promotes to type names (`_Must_inspect_result_`
// expands to nothing, leaving `using = void*;`, also C2059). Emitted nowhere
// and referenced nowhere, so skipping is safe.
static bool is_valid_alias_name(std::string const& n)
{
    static const std::set<std::string> kw = {
        "void","char","short","int","long","float","double","signed","unsigned",
        "bool","wchar_t","char8_t","char16_t","char32_t","auto","decltype",
        "struct","class","union","enum","const","volatile","typedef","static",
        "extern","register","template","typename","operator","new","delete",
        "this","using","namespace","goto","sizeof","alignas","return","if","else",
        "switch","case","default","break","continue","for","while","do","try",
        "catch","throw","public","private","protected","virtual","friend","inline",
        "export","module","import","nullptr","true","false",
        // MSVC type/keyword tokens leaked as identifiers or alias targets
        "__unaligned","__ptr32","__ptr64","__sptr","__uptr","__w64","__based",
        "__segment","__declspec","__forceinline","__inline","__asm","__wchar_t",
        "__int8","__int16","__int32","__int64","__leave","__try","__finally","__except",
        // C++ alternative-operator tokens
        "and","or","not","xor","bitand","bitor","compl","and_eq","or_eq","not_eq","xor_eq"
    };
    if (kw.count(n)) return false;
    // SAL annotation debris promoted to a type name
    static const char* sal_prefixes[] = {
        "_Must_","_Out_","_In_","_Inout_","_Ret_","_When_","_Post_","_Pre_",
        "_Null_","_Notnull_","_Check_","_Deref_","_Field_","_Success_","_At_",
        "_Use_decl_annotations_","_Printf_format_string_","_Null_terminated_"
    };
    for (auto const* p : sal_prefixes)
        if (n.rfind(p, 0) == 0) return false;
    return true;
}

static void write_win32_struct(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const&)
{
    static const std::set<std::string> cxx_type_kw = {
        "void","char","short","int","long","float","double","signed","unsigned",
        "bool","struct","class","union","enum","const","volatile","typedef"
    };
    if (cxx_type_kw.count(type.name)) return;
    // Empty structs from winversec typedefs (e.g. WCHAR, DWORD) — skip
    if (type.fields.empty()) return;
    // Struct/union fields parsed from macro-heavy C headers can carry C++
    // keyword names (e.g. RPC ~~union~~ field `char`, `const` parse debris);
    // emitting them verbatim is a hard syntax error, so mangle them.
    auto field_name = [](std::string const& n) -> std::string
    {
        static const std::set<std::string> cxx_kw = {
            "void","char","short","int","long","float","double","signed","unsigned",
            "bool","struct","class","union","enum","const","volatile","typedef",
            "default","auto","register","template","typename","operator","new","delete",
            "this","using","namespace","static","extern","goto","sizeof","alignas"
        };
        if (cxx_kw.count(n)) return n + "_";
        return n;
    };
    // Paired #ifdef branches in C headers define the same union field twice
    // (HSTRING_HEADER's Reserved2); the flattened metadata then carries the
    // duplicate and the emission is a hard C2086 redefinition. Keep first.
    std::set<std::string> seen_fields;
    auto unique_name = [&](std::string const& n) -> std::string
    {
        std::string mangled = field_name(n);
        while (!seen_fields.insert(mangled).second)
            mangled += "_";
        return mangled;
    };
    // A bare void VALUE field is parse debris (no C or C++ object can have
    // type void): emitting it is a hard C2182. Skip it everywhere the
    // struct body is written.
    auto is_void_field = [](winmd::field_info const& f)
    {
        return f.value.kind == winmd::et_void
            && f.value.children.empty() && f.value.fn_params.empty();
    };
    if (type.struct_union)
    {
        w.write("export struct %\n{\n    union\n    {\n", type.name);
        for (auto const& f : type.fields)
        {
            if (is_void_field(f)) continue;
            if (f.arr_n > 0)
                w.write("        % %[%];\n", mapper.map(f.value), unique_name(f.name), f.arr_n);
            else
                w.write("        % %;\n", mapper.map(f.value), unique_name(f.name));
        }
        w.write("    };\n};\n\n");
    }
    else
    {
        if (type.struct_pack > 0) w.write("#pragma pack(push, %)\n", type.struct_pack);
        w.write("export struct %\n{\n", type.name);
        for (auto const& f : type.fields)
        {
            if (is_void_field(f)) continue;
            if (f.arr_n > 0)
                w.write("    % %[%];\n", mapper.map(f.value), unique_name(f.name), f.arr_n);
            else
                w.write("    % %;\n", mapper.map(f.value), unique_name(f.name));
        }
        w.write("};\n");
        if (type.struct_pack > 0) w.write("#pragma pack(pop)\n");
        w.write("\n");
    }
}

static std::set<std::string> make_cxx_kw_set()
{
    return {
        "long","int","short","char","float","double","void","bool","class","struct",
        "union","enum","const","volatile","static","extern","return","break","continue",
        "switch","case","default","if","else","for","while","do","goto","sizeof","typedef",
        "new","delete","try","catch","throw","operator","this","true","false","namespace",
        "using","template","typename","register","auto","signed","unsigned",
        "wchar_t","char8_t","char16_t","char32_t","nullptr","constexpr","decltype","noexcept",
        // C++ alternative-operator tokens: SDK enums carry `or`/`not`/`and`
        // as member names (ICU, TEXTSERV); they are keywords, not identifiers.
        "and","or","not","xor","bitand","bitor","compl","and_eq","or_eq","not_eq","xor_eq",
        "_","in","out","inout","opt","optional","reserved","result","buffer",
        "size","length","count","index","value","type","name","handle","flags",
        "constant","near","far","pascal","cdecl","stdcall"
    };
}

static std::string sanitize_module_name(std::string const& name)
{
    // Normalize path-ish characters, then mangle each dot segment the same
    // way the namespace writer does (BITS1.5 -> BITS1_5) so the exported
    // module name and its importers always agree.
    std::string result = name;
    for (auto& c : result)
    {
        if (c == ':' || c == '<' || c == '>' || c == '/' || c == '\\')
            c = '.';
    }
    auto segs = mangle_ns_segments(result);
    result.clear();
    for (auto const& s : segs)
    {
        if (!result.empty())
        {
            result += '.';
        }
        result += s;
    }
    return result;
}

static std::string sanitize_param_name(std::string pname, std::set<std::string>& seen)
{
    static const auto cxx_kw = make_cxx_kw_set();
    // Windows header macros/type tokens leaked as parameter names from the
    // metadata (VOID, LPCSTR, DWORD, ...): several are preprocessor macros
    // (VOID -> void) that turn the emitted signature into a syntax error.
    // Any all-caps token is treated as a leaked type token, never a real
    // parameter name (real Win32/WDK parameter names are camelCase).
    static const std::set<std::string> win32_type_tokens = {
        "VOID", "LPCSTR", "LPCWSTR", "LPSTR", "LPWSTR", "LPVOID", "LPCVOID",
        "DWORD", "WORD", "BYTE", "WCHAR", "CHAR", "UINT", "INT", "BOOL",
        "HANDLE", "HWND", "ULONG", "LONG", "SHORT", "FLOAT", "PVOID",
        "PWSTR", "PCWSTR", "PSTR", "LPBOOL", "LPDWORD", "SIZE_T", "SSIZE_T",
        "HRESULT", "NTSTATUS", "BOOLEAN", "COLORREF", "LPARAM", "WPARAM", "LRESULT"
    };
    bool leaked = pname.empty();
    if (!leaked && pname[0] >= 'A' && pname[0] <= 'Z')
    {
        leaked = true;
        for (char c : pname)
        {
            if ((c < 'A' || c > 'Z') && c != '_' && (c < '0' || c > '9')) { leaked = false; break; }
        }
    }
    if (win32_type_tokens.count(pname)) leaked = true;
    if (leaked)
    {
        // arg / arg2 / arg3 ... — unique within the parameter list
        std::string candidate = "arg";
        int n = 2;
        while (seen.count(candidate)) candidate = "arg" + std::to_string(n++);
        seen.insert(candidate);
        return candidate;
    }
    if (cxx_kw.count(pname) || pname[0] == '_') pname = "_" + pname;
    std::string base = pname;
    int suffix = 2;
    while (!seen.insert(pname).second)
        pname = base + std::to_string(suffix++);
    return pname;
}

// A C signature cannot say `void x` for an untyped parameter (C2860
// 'void' cannot be used as a function parameter except for '(void)'):
// metadata that carries no parameter type maps to void, so project such
// parameters as void* (an opaque byte pointer, the C convention).
static std::string param_type_cpp(type_mapper const& mapper, winmd::param_info const& param)
{
    std::string t = mapper.map(param.value);
    if (t == "void") return "void*";
    return t;
}

static void write_win32_delegate(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const&, std::set<std::string>& emitted)
{
    // C++ keywords cannot be alias targets (`using wchar_t = ...;` is a hard
    // C2059). RPCNDR's scalar typedefs (byte, boolean, wchar_t, size_t) get
    // misclassified as delegates; the keyword ones must not be emitted — the
    // keyword is natively available to consumers.
    if (!is_valid_alias_name(type.name)) return;
    if (type.invoke < 0 || type.invoke >= static_cast<std::int32_t>(type.methods.size()))
    {
        // Delegate without a usable Invoke signature (winversec parse debris
        // from `typedef void (NTAPI *PFOO)(...)` chains where only the
        // P-prefixed name has a body): emit the opaque pointer alias so
        // struct fields and params typed with the bare name still resolve
        // (RTL_UMS_SCHEDULER_ENTRY_POINT, EXCEPTION_ROUTINE fields failed
        // with C2039 when the name was left undefined).
        if (emitted.insert(type.name).second)
            w.write("export using % = void*;\n\n", type.name);
        return;
    }
    // Reserve the name so the flat-function pass never emits a C function
    // with the same identifier (a using-alias and a function cannot share a
    // name in one namespace).
    emitted.insert(type.name);
    auto const& method = type.methods[type.invoke];
    std::string cc = "__stdcall";
    switch (type.calling_convention) { case 1: cc = "__cdecl"; break; case 3: cc = "__thiscall"; break; case 4: cc = "__fastcall"; break; }
    std::string params;
    std::set<std::string> seen;
    for (std::size_t p = 0; p < method.params.size(); ++p)
    {
        if (p) params += ", ";
        std::string pname = sanitize_param_name(method.params[p].name, seen);
        params += param_type_cpp(mapper, method.params[p]) + " " + pname;
    }    
    // C++ function-pointer type: ret(ccall*)(params) — the '*' binds to the
    // declarator, and an empty param list still needs its parentheses.
    // `export` is mandatory: a named type alias is not visible to importing
    // modules without it (winrt_base's imported PVECTORED_EXCEPTION_HANDLER
    // and friends failed with C2061 until this).
    if (type.is_driver_dispatch) w.write("// DRIVER_DISPATCH callback\n");
    w.write("export using % = %(%*)(%);\n\n", type.name, mapper.map(method.ret), cc, params);
}

// Linear, non-throwing scanners for the component metadata sweeps over
// generated .ixx files. These replace std::regex: MSVC's std::regex threw
// regex_error(error_complexity) part-way through the sweep, and because the
// sweep sits inside a best-effort heuristic (discovering which properties /
// base members the generated views call) that exception aborted the whole
// -component run with no module diagnostics. Same language as the original
// patterns, O(n) per line, cannot throw.
template <typename F>
static void for_each_prop_call(std::string const& line, F&& fn)
{
    auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::size_t i = 0;
    while (i < line.size())
    {
        std::size_t a = i;
        while (a < line.size() && is_word(line[a])) ++a;
        if (a == i) { ++i; continue; }
        // group1 ends here only if a separator follows: (\w+)[_.](\w+)\s*\(
        if (a < line.size() && (line[a] == '_' || line[a] == '.'))
        {
            std::size_t b = a + 1;
            std::size_t be = b;
            while (be < line.size() && is_word(line[be])) ++be;
            if (be > b)
            {
                std::size_t k = be;
                while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
                if (k < line.size() && line[k] == '(')
                {
                    fn(line.substr(i, a - i), line.substr(b, be - b));
                    i = k + 1;
                    continue;
                }
            }
        }
        // No match can start inside this word run either: any inner start
        // would end group1 at the same `a` and hit the same failing tail.
        i = a;
    }
}

template <typename F>
static void for_each_call_pattern(std::string const& line, F&& fn)
{
    auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    // \b(\w+)_\d+\.(\w+)\s*\( — anchor on the "_digits." separator, then
    // walk back for the greedy group1 and forward for group2.
    std::size_t q = 0;
    while ((q = line.find('_', q)) != std::string::npos)
    {
        std::size_t d = q + 1;
        while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
        if (d == q + 1 || d >= line.size() || line[d] != '.') { ++q; continue; }
        std::size_t s = q;
        while (s > 0 && is_word(line[s - 1])) --s;
        if (s == q) { ++q; continue; }              // group1 needs >= 1 char
        if (s > 0 && is_word(line[s - 1])) { ++q; continue; }  // \b boundary
        std::size_t b = d + 1;
        std::size_t be = b;
        while (be < line.size() && is_word(line[be])) ++be;
        if (be == b) { ++q; continue; }
        std::size_t k = be;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        if (k >= line.size() || line[k] != '(') { ++q; continue; }
        fn(line.substr(s, q - s), line.substr(b, be - b));
        q = k + 1;
    }
}

// One emitted function signature, retained for the A/W canonical pass.
struct aw_fn_sig
{
    std::string ret;
    std::string params_text;                                   // rendered "type name, ..."
    std::vector<std::pair<std::string, std::string>> params;   // (type, name)
    bool is_stub = false;                                      // deferred inline stub
};// A/W canonical wrappers. The Windows SDK ships Unicode APIs as paired
// FooA/FooW entry points; the projection emits both, so callers would have
// to know the metadata's spelling to call anything. For every A/W pair in a
// namespace, emit one unsuffixed wrapper that forwards to the W (Unicode)
// variant — `CreateFile`, `RegOpenKeyEx`, `RegisterClass`. The wrapper is
// `export inline`: a plain inline is invisible across the module boundary
// and every consumer fails C3861.
// Skipped when the canonical name is already a real function in the
// namespace; a metadata-synthesized canonical STUB (winversec's GetMessage
// `return {};`) does not count — the stub is suppressed below and the
// forwarder takes its place.
// String parameters keep a wide spelling in the canonical wrapper: the
// metadata flattens LPCWSTR/LPTSTR parameters to void*, which a bare L"..."
// argument does not convert to, so the wrapper takes wchar_t const* /
// wchar_t* and casts at the forwarding call.
// When emit_deferred_stubs is set (win32 writer, which defers its inline
// stubs to here), every collected stub that no canonical replaced is
// emitted after the canonicals, keeping its module-internal definition.
static void write_aw_canonical(writer& w, std::string const& ns_cpp,
    std::vector<std::pair<std::string, aw_fn_sig>> const& aw_fns,
    bool emit_deferred_stubs = false)
{
    std::set<std::string> canonical_bases;
    // Canonical wrappers are BUFFERED and flushed after the deferred stubs:
    // a wrapper's qualified forward call (::Ns::FooW) needs FooW already
    // declared in this module, and the deferred pass is what declares it
    // (a bare forward before the declaration fails C2039 "not a member").
    writer cw;
    for (auto const& [a_name, a_sig] : aw_fns)
    {
        // Keyed on the A half: name must end in 'A' with a name char before.
        if (a_name.size() < 2 || a_name.back() != 'A') continue;
        if (!std::isalpha(static_cast<unsigned char>(a_name[a_name.size() - 2]))) continue;
        std::string base = a_name.substr(0, a_name.size() - 1);
        std::string wide_name = base + "W";
        aw_fn_sig const* wide = nullptr;
        for (auto const& [n, s] : aw_fns) if (n == wide_name) { wide = &s; break; }
        if (!wide) continue;
        bool canonical_taken = false;
        for (auto const& [n, s] : aw_fns) if (n == base && !s.is_stub) { canonical_taken = true; break; }
        if (canonical_taken) continue;
        if (a_sig.ret != wide->ret) continue;
        if (a_sig.params.size() != wide->params.size()) continue;

        std::string cparams, cargs;
        for (std::size_t i = 0; i < wide->params.size(); ++i)
        {
            std::string ptype = wide->params[i].first;
            std::string const& atype = a_sig.params[i].first;
            std::string const& pname = wide->params[i].second;
            bool retyped = false;
            // Narrow string in A + void* in W is the metadata's
            // (narrow-string, wide-string) parameter pair — spell it wide.
            if (ptype == "void*" && atype == "std::int8_t const*") { ptype = "wchar_t const*"; retyped = true; }
            else if (ptype == "void*" && atype == "std::int8_t*") { ptype = "wchar_t*"; retyped = true; }
            if (i) { cparams += ", "; cargs += ", "; }
            cparams += ptype + " " + pname;
            cargs += (retyped ? "(void*)" : "") + pname;
        }
        cw.write("// A/W canonical wrapper -> %\n", wide_name);
        if (wide->ret == "void")
            cw.write("export inline void % (%)\n{\n    ::%::%(%);\n}\n\n", base, cparams, ns_cpp, wide_name, cargs);
        else
            cw.write("export inline % % (%)\n{\n    return ::%::%(%);\n}\n\n", wide->ret, base, cparams, ns_cpp, wide_name, cargs);
        canonical_bases.insert(base);
    }
    if (emit_deferred_stubs)
    {
        for (auto const& [n, s] : aw_fns)
        {
            if (!s.is_stub) continue;
            if (canonical_bases.count(n)) continue;  // replaced by the forwarder
            if (s.ret == "void")
                w.write("export inline void % (%)\n{\n}\n\n", n, s.params_text);
            else
                w.write("export inline % % (%)\n{\n    return {};\n}\n\n", s.ret, n, s.params_text);
        }
    }
    w.write("%", cw.str());
}

// C standard library names that `import std;` already declares in the
// global scope. Re-declaring them `extern "C"` with winmd-derived
// parameters collides with the real declaration: C linkage ignores
// namespaces, so the projection's `void* __Src` (metadata drops const)
// cannot coexist with <string.h>'s `void const* __Src` -> C2116/C2733 in
// the module AND in every importer that sees both modules. Emit a
// using-declaration instead: the namespace keeps the name, the call
// resolves to the genuine UCRT symbol with correct const/ABI, and /MD's
// default ucrt.lib already provides it. Every name here is declared by a
// C standard header (guaranteed part of import std); non-CRT names
// (Win32/WDK APIs - never declared by std) keep the extern decl.
static bool crt_global_alias(std::string const& name)
{
    static const std::set<std::string> crt = {
        // string.h / wchar.h — proven C2116/C2733 conflicts
        "memcmp", "memcpy", "memmove", "strcmp", "strncmp", "strcoll",
        "strcspn", "strerror", "strlen", "strnlen", "strspn", "strtok",
        "strxfrm", "wcscmp", "wcscoll", "wcscspn", "wcslen", "wcsnlen",
        "wcsncmp", "wcsspn", "wcsxfrm",
        // same families, same const-vs-void* shape (other modules)
        "strcpy", "strncpy", "strcat", "strncat", "strchr", "strrchr",
        "strstr", "strpbrk", "wcscpy", "wcsncpy", "wcscat", "wcsncat",
        "wcschr", "wcsrchr", "wcsstr", "wcspbrk", "wmemcpy", "wmemmove",
        "wmemset", "wmemcmp", "wmemchr", "wcsrtombs", "mbsrtowcs",
        // stdio.h — proven conflicts
        "clearerr", "fclose", "feof", "ferror", "fflush", "fgetc",
        "fgetpos", "fgets", "fopen", "fputc", "fputs", "fread",
        "freopen", "fsetpos", "fseek", "ftell", "fwrite", "getc",
        "perror", "putc", "puts", "remove", "rename", "rewind",
        "setbuf", "setvbuf", "tmpfile", "ungetc", "fgetwc", "fputwc",
        "getwc", "putwc", "ungetwc", "fgetws", "fputws", "fwprintf",
        // stdlib.h — proven conflicts
        "labs", "atexit", "atof", "atoi", "atol", "bsearch", "qsort",
        "getenv", "ldiv", "mblen", "mbtowc", "strtod", "strtol",
        "strtoul", "system", "wctomb", "wcstod", "wcstol", "wcstoul",
        // time.h/wchar.h/setjmp.h/eh.h — proven conflicts (the projected
        // params name PROJECTED types — Wdk::TIME::tm, void* instead of
        // jmp_buf/terminate_handler — so the extern "C" redeclaration
        // mismatches the import-std-visible CRT declaration: C2116/C2733,
        // C2382 exception-spec on terminate)
        "wcsftime", "mbrlen", "mbrtowc", "longjmp", "terminate", "set_terminate",
    };
    return crt.count(name) != 0;
}

static void write_win32_function(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const&, std::set<std::string>& emitted, std::vector<std::pair<std::string, aw_fn_sig>>& aw_acc)
{
    if (type.methods.empty()) return;
    std::string cc = "__stdcall";
    switch (type.calling_convention) { case 1: cc = "__cdecl"; break; case 3: cc = "__thiscall"; break; case 4: cc = "__fastcall"; break; }
    static const std::set<std::string> skip_names = {
        "NTSTATUS","TAG","DECLSPEC_UUID","DECLSPEC_UUIDX",
        "data","resource","local","align","pack"
    };
    for (auto const& method : type.methods)
    {
        std::string fname = method.name.empty() ? type.name : method.name;
        if (!fname.empty() && fname[0] == '_') continue;
        if (skip_names.count(fname)) continue;
        if (fname.empty() || std::isdigit(static_cast<unsigned char>(fname[0]))) continue;
        static const auto cxx_kw_fn = make_cxx_kw_set();
        if (cxx_kw_fn.count(fname)) continue;
        std::string ret = mapper.map(method.ret);
        std::string params;
        std::set<std::string> seen;
        std::vector<std::pair<std::string, std::string>> sig_params;
        for (std::size_t p = 0; p < method.params.size(); ++p)
        {
            if (p) params += ", ";
            std::string pname = sanitize_param_name(method.params[p].name, seen);
            std::string ptype = param_type_cpp(mapper, method.params[p]);
            params += ptype + " " + pname;
            sig_params.push_back({ std::move(ptype), pname });
        }
        std::string sig_key = fname;
        if (!emitted.insert(sig_key).second) continue;
        // Inline duplicate of a REAL dllimport API (winbase.h's _M_CEE
        // block re-declares FormatMessage/LocalFree with __inline bodies;
        // after macro expansion the inline copy wins the row). A mapped
        // import library means the export exists - emit the extern decl so
        // consumers (winrt_base) link it, not the empty stub.
        bool is_stub = type.is_inline_function && type.import_library.empty();
        aw_acc.push_back({ fname, { ret, params, std::move(sig_params), is_stub } });
        // Deferred to the namespace-supplements pass: it emits the stub after
        // the A/W canonical wrappers, which may replace it outright.
        if (is_stub) continue;
        // `export extern "C"`: without the export keyword the function
        // declaration is not visible across the module boundary and every
        // consumer fails with C3861 'identifier not found'.
        // CRT row: `import std;` already declares this name globally —
        // alias it instead of a conflicting second declaration.
        if (crt_global_alias(fname))
            w.write("export using ::%;\n", fname);
        else
            w.write("export extern \"C\" % % %(%);\n", ret, cc, fname, params);
        if (!type.import_library.empty()) w.write("// Import library: %\n", type.import_library);
        if (type.last_error) w.write("// SetLastError: true\n");
        if (type.callback_optional) w.write("// Optional callback parameter (may be nullptr)\n");
        if (type.is_driver_dispatch) w.write("// DRIVER_DISPATCH callback\n");
        w.write("\n");
    }
}

// Format 16 GUID bytes as a GUID brace initializer:
// { Data1, Data2, Data3, { Data4[0..7] } } (fields little-endian).
static std::string guid_brace_bytes(std::uint8_t const* g)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf),
        "0x%08X, 0x%04X, 0x%04X, { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X }",
        (std::uint32_t)g[0] | ((std::uint32_t)g[1] << 8) | ((std::uint32_t)g[2] << 16) | ((std::uint32_t)g[3] << 24),
        (unsigned)(g[4] | (g[5] << 8)), (unsigned)(g[6] | (g[7] << 8)),
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

static void write_win32_com_interface(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const&)
{
    std::string cn = type.name;
    w.write("export struct %\n{\n", cn);
    w.write("    void* m_ptr = nullptr;\n");
    w.write("    %() noexcept = default;\n", cn);
    w.write("    %(void* p) noexcept : m_ptr(p) {}\n", cn);
    // Self-contained IUnknown::Release via vtable slot 2 (QI=0, AddRef=1,
    // Release=2). Win32/WDK modules must NOT depend on winrt::IUnknown —
    // winrt_base is never generated in Win32-only runs (x64: __stdcall is
    // the only calling convention, matching the slot function pointer).
    w.write("    ~%() { if (m_ptr) { void** v = *reinterpret_cast<void***>(m_ptr);"
            " reinterpret_cast<unsigned long (__stdcall*)(void*)>(v[2])(m_ptr); m_ptr = nullptr; } }\n", cn);
    w.write("    void* get_abi() const noexcept { return m_ptr; }\n");
    w.write("    explicit operator bool() const noexcept { return m_ptr != nullptr; }\n");
    for (auto const& method : type.methods)
    {
        std::string params;
        std::set<std::string> seen;
        for (std::size_t p = 0; p < method.params.size(); ++p)
        {
            if (p) params += ", ";
            std::string pname = sanitize_param_name(method.params[p].name, seen);
            params += param_type_cpp(mapper, method.params[p]) + " " + pname;
        }
        w.write("    % %(%) const;\n", mapper.map(method.ret), method.name, params);
    }
    w.write("};\n\n");
    if (type.com_interface && type.has_com_iid)
        w.write("inline constexpr % IID_% = { % };\n\n",
            // Globally anchored: inside a flat-C WASDK module (namespace
            // Windows::Win32::AppSDK::X) a bare "Windows::..." reference
            // walks outward through the enclosing scopes instead of binding
            // at global scope — C3083/C2039 on every IID_* constant. Same
            // anchoring as win32_type_cpp_name.
            "::Windows::Win32::GUIDDEF::GUID", cn, guid_brace_bytes(type.com_iid));
}

static bool is_win32_function(winmd::type_info const& type)
{
    return is_win32_type(type) && type.kind == winmd::type_kind::class_;
}

static bool is_win32_com(winmd::type_info const& type)
{
    if (!is_win32_type(type) || type.kind != winmd::type_kind(1)) return false; // interface = 1
    for (auto const* iface : type.interfaces)
        if (iface && (iface->name == "IUnknown" || iface->name == "IDispatch")) return true;
    return false;
}

//
// WDK/Driver API projection writers
//

enum class wdk_driver_model { unknown, wdm, kmdf, umdf, wdf };

static wdk_driver_model classify_driver_model(winmd::type_info const& type)
{
    // Namespace heuristics are primary — the DriverType attribute from
    // the .winmd may be set to a default value (e.g. UMDF=2) that doesn't
    // reflect the actual kernel/user-mode split of individual APIs.
    auto ns = type.ns;
    if (ns.starts_with("Windows.Win32")) return wdk_driver_model::umdf;
    if (ns.find("KernelMode") != std::string_view::npos) return wdk_driver_model::wdm;
    if (ns.find("Kmdf") != std::string_view::npos || ns.find("KMDF") != std::string_view::npos) return wdk_driver_model::kmdf;
    if (ns.find("Umdf") != std::string_view::npos || ns.find("UMDF") != std::string_view::npos) return wdk_driver_model::umdf;
    if (ns.find("Wdf") != std::string_view::npos) return wdk_driver_model::kmdf;
    // Windows.Wdk.* without a specific driver model marker → WDM
    if (ns.starts_with("Windows.Wdk")) return wdk_driver_model::wdm;
    // Fall back to DriverType attribute when namespace is ambiguous
    if (type.driver_type > 0)
    {
        switch (type.driver_type) { case 1: return wdk_driver_model::kmdf; case 2: return wdk_driver_model::umdf; case 3: return wdk_driver_model::wdm; }
    }
    // Default: WDM (most WDK kernel functions)
    return wdk_driver_model::wdm;
}

static void write_wdk_enum(writer& w, winmd::type_info const& type, type_mapper const& mapper)
{
    if (type.has_irp_major_function)
        w.write("// IRP major function code: %\n", type.irp_major_function);
    if (type.ioctl_code)
        w.write("// IOCTL code: % (buffering method %)\n", type.ioctl_code, type.ioctl_buffering);
    std::string underlying = enum_underlying_cpp(type.enum_underlying);
    if (underlying.empty()) underlying = "std::int32_t";
    // Singleton "const enums" -> exported constants (see write_win32_enum).
    if (type.enum_fields.size() == 1 && type.enum_fields[0].first == type.name
        && !type.ioctl_code && !type.has_irp_major_function)
    {
        // GENERIC_READ (0x80000000) and the predefined HKEY_* roots
        // (0x80000001) don't fit the default int32 underlying — a literal
        // wider than its declared type is ill-formed (C2397). The
        // materializer tags every constant field U4, so widen when the
        // value needs it.
        std::string const_underst = underlying;
        if (type.enum_fields[0].second > 2147483647LL ||
            type.enum_fields[0].second < -2147483648LL)
            const_underst = "std::uint32_t";
        // #undef first: BUFSIZ/EXIT_SUCCESS/FILENAME_MAX/RAND_MAX are CRT
        // macros visible in this TU — without it the token expands to a
        // literal before the declaration parses (C2332/C2059). Macros never
        // cross a module boundary, so importers never see this name anyway.
        w.write("#undef %\n", mangle_enum_member(type.name));
        w.write("export inline constexpr % % = %;\n\n", const_underst,
            mangle_enum_member(type.name), type.enum_fields[0].second);
        return;
    }
    // Same macro guard as the singleton-const path above.
    w.write("#undef %\n", type.name);
    w.write("export enum class % : %\n{\n", type.name, underlying);
    // Keep the first definition of each enumerator name. Some SDK headers
    // define the same enumerator twice (e.g. MSI's SCRIPTFLAGS_REGDATA_APPINFO
    // = 0 then = 16); emitting both is a C2086 redefinition error.
    {
        std::set<std::string> seen_enum;
        for (auto const& [name, value] : type.enum_fields)
        {
            if (!seen_enum.insert(name).second) continue;
            w.write("    % = %,\n", mangle_enum_member(name), value);
        }
    }
    w.write("};\n\n");
    if (type.enum_flags)
    {
        w.write("export constexpr % operator|(% a, % b) noexcept { return static_cast<%>(static_cast<%>(a) | static_cast<%>(b)); }\n",
            type.name, type.name, type.name, type.name, underlying, underlying);
        w.write("export constexpr % operator&(% a, % b) noexcept { return static_cast<%>(static_cast<%>(a) & static_cast<%>(b)); }\n",
            type.name, type.name, type.name, type.name, underlying, underlying);
        w.write("export constexpr % operator~(% a) noexcept { return static_cast<%>(~static_cast<%>(a)); }\n\n",
            type.name, type.name, type.name, underlying);
    }
}

static void write_wdk_struct(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const&)
{
    auto field_name = [](std::string const& n) -> std::string
    {
        static const std::set<std::string> cxx_kw = {
            "void","char","short","int","long","float","double","signed","unsigned",
            "bool","struct","class","union","enum","const","volatile","typedef",
            "default","auto","register","template","typename","operator","new","delete",
            "this","using","namespace","static","extern","goto","sizeof","alignas"
        };
        if (cxx_kw.count(n)) return n + "_";
        return n;
    };
    // Paired #ifdef branches in C headers define the same union field twice
    // (HSTRING_HEADER's Reserved2); the flattened metadata then carries the
    // duplicate and the emission is a hard C2086 redefinition. Keep first.
    std::set<std::string> seen_fields;
    auto unique_name = [&](std::string const& n) -> std::string
    {
        std::string mangled = field_name(n);
        while (!seen_fields.insert(mangled).second)
            mangled += "_";
        return mangled;
    };
    // A bare void VALUE field is parse debris (no C or C++ object can have
    // type void): emitting it is a hard C2182. Skip it everywhere the
    // struct body is written.
    auto is_void_field = [](winmd::field_info const& f)
    {
        return f.value.kind == winmd::et_void
            && f.value.children.empty() && f.value.fn_params.empty();
    };
    if (type.struct_union)
    {
        w.write("export struct %\n{\n    union\n    {\n", type.name);
        for (auto const& f : type.fields)
        {
            if (is_void_field(f)) continue;
            if (f.arr_n > 0)
                w.write("        % %[%];\n", mapper.map(f.value), unique_name(f.name), f.arr_n);
            else
                w.write("        % %;\n", mapper.map(f.value), unique_name(f.name));
        }
        w.write("    };\n};\n\n");
    }
    else
    {
        if (type.struct_pack > 0) w.write("#pragma pack(push, %)\n", type.struct_pack);
        w.write("export struct %\n{\n", type.name);
        for (auto const& f : type.fields)
        {
            if (is_void_field(f)) continue;
            if (f.arr_n > 0)
                w.write("    % %[%];\n", mapper.map(f.value), unique_name(f.name), f.arr_n);
            else
                w.write("    % %;\n", mapper.map(f.value), unique_name(f.name));
        }
        w.write("};\n");
        if (type.struct_pack > 0) w.write("#pragma pack(pop)\n");
        w.write("\n");
    }
}

// Which SDK lib tree carries an import library — "um", "ucrt", "km", or
// "" (not shipped by the SDK). Drives write_wdk_function's stub-vs-extern
// decision: a lib found only under the kernel tree (ntoskrnl.lib,
// storport.lib, wdfldr.lib) can never be linked into a user-mode binary —
// importing a .sys/.exe image breaks the process load — while the WDK's
// user-callable headers resolve to um/ libraries (rpcdce.h -> rpcrt4.lib,
// tbs.h -> tbs.lib) that must link exactly like Win32. The attribution may
// spell the DLL (wdfldr.dll), a driver image (ntoskrnl.exe) or the lib
// itself (rpcrt4.lib); the lib stem is probed in all three spellings.
// Cached per lib name; scans the newest SDK like the compile driver does.
static std::string probe_sdk_lib_dir(std::string const& lib)
{
    static std::unordered_map<std::string, std::string> cache;
    if (auto it = cache.find(lib); it != cache.end()) return it->second;
    std::string result;
    std::error_code ec;
    std::filesystem::path kits;
    if (const char* pf = std::getenv("ProgramFiles(x86)"))
        kits = std::filesystem::path(pf) / "Windows Kits" / "10" / "Lib";
    else if (const char* pf = std::getenv("ProgramFiles"))
        kits = std::filesystem::path(pf) / "Windows Kits" / "10" / "Lib";
    if (!kits.empty())
    {
        std::filesystem::path newest;
        for (auto const& e : std::filesystem::directory_iterator(kits, ec))
        {
            if (!e.is_directory(ec)) continue;
            std::string fname = e.path().filename().string();
            if (fname.empty() || !std::isdigit(static_cast<unsigned char>(fname.front()))) continue;
            if (newest.empty() || fname > newest.filename().string()) newest = e.path();
        }
        if (!newest.empty())
        {
            std::string name = lib;
            auto dot = name.rfind('.');
            std::string stem = (dot == std::string::npos) ? name : name.substr(0, dot);
            auto probe_stem = [&](std::string const& s) -> std::string
            {
                for (auto const* sub : { "um", "ucrt", "km" })
                {
                    if (std::filesystem::exists(newest / sub / "x64" / (s + ".lib"), ec))
                        return sub;
                }
                return {};
            };
            result = probe_stem(stem);
            // DLL-name attributions whose SDK import library ships under a
            // DIFFERENT stem: the WDK's user-mode CRT headers dllimport
            // "ucrtbase.dll", but the SDK ships that surface as ucrt.lib —
            // no ucrtbase.lib exists in any tree. Without this alias every
            // CRT API (malloc/printf/strcmp/fopen...) looked lib-less and
            // fell into the kernel-stub path, compiling user-mode code to
            // silently-broken empty stubs (malloc returning nullptr).
            if (result.empty())
            {
                static const std::unordered_map<std::string, std::string> stem_alias = {
                    { "ucrtbase", "ucrt" },   // ucrtbase.dll -> ucrt.lib
                };
                if (auto ait = stem_alias.find(stem); ait != stem_alias.end())
                    result = probe_stem(ait->second);
            }
        }
    }
    cache.emplace(lib, result);
    return result;
}

static void write_wdk_function(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const&, std::set<std::string>& emitted, std::vector<std::pair<std::string, aw_fn_sig>>& aw_acc)
{
    if (type.methods.empty()) return;
    wdk_driver_model model = classify_driver_model(type);
    std::string cc = (model == wdk_driver_model::wdm) ? "__stdcall" : "";
    std::string lib_name;
    switch (model)
    {
    case wdk_driver_model::wdm:  lib_name = "ntoskrnl.exe"; cc = "__stdcall"; break;
    case wdk_driver_model::kmdf: lib_name = "wdfldr.dll"; break;
    case wdk_driver_model::umdf: lib_name = "wdfumdf.dll"; break;
    default: lib_name = "ntoskrnl.exe"; cc = "__stdcall"; break;
    }
    if (!type.import_library.empty())
    {
        // Truthful body comment on whatever stub survives: the projection
        // now attributes kernel rows too (km scan / driver fill), so show
        // the real library rather than the model default.
        lib_name = type.import_library;
    }
    static const std::set<std::string> skip_names = {
        "NTSTATUS", "TAG", "DECLSPEC_UUID", "DECLSPEC_UUIDX",
        "data", "resource", "local", "align", "pack"
    };
    for (auto const& method : type.methods)
    {
        std::string fname = method.name.empty() ? type.name : method.name;
        if (!fname.empty() && fname[0] == '_') continue;
        if (skip_names.count(fname)) continue;
        if (fname.empty() || std::isdigit(static_cast<unsigned char>(fname[0]))) continue;
        static const auto cxx_kw_wdk = make_cxx_kw_set();
        if (cxx_kw_wdk.count(fname)) continue;
        // C functions cannot overload: the same API name may surface from
        // several metadata wrapper types — emit the first declaration only.
        if (!emitted.insert(fname).second) continue;
        std::string ret = mapper.map(method.ret);
        std::string params;
        std::set<std::string> seen;
        std::vector<std::pair<std::string, std::string>> sig_params;
        for (std::size_t p = 0; p < method.params.size(); ++p)
        {
            if (p) params += ", ";
            std::string pname = sanitize_param_name(method.params[p].name, seen);
            std::string ptype = param_type_cpp(mapper, method.params[p]);
            params += ptype + " " + pname;
            sig_params.push_back({ std::move(ptype), pname });
        }
        aw_acc.push_back({ fname, { ret, params, std::move(sig_params), false } });
        if (type.is_driver_dispatch) w.write("// DRIVER_DISPATCH callback\n");
        if (type.callback_optional) w.write("// Optional callback parameter (may be nullptr)\n");
        // User-mode API with a real import library: the symbol exists in the
        // DLL, so emit the real extern decl - an inline stub is invisible
        // across the module boundary and consumers fail C2039 (winrt_base's
        // ss::FormatMessageW / ss::LocalFree). Mirrors write_win32_function's
        // rule for Windows.Win32.* rows. WDK namespaces and
        // Windows.Win32.Kernel.* used to ALWAYS stub, discarding correct
        // attributions for the WDK headers that export user-callable APIs
        // (rpcdce.h -> rpcrt4.lib, tbs.h -> tbs.lib). The SDK lib tree
        // decides now: um/ or ucrt/ attribution links for real; kernel-tree
        // libs and unattributed rows keep the intentional kernel stub —
        // linking a .sys/.exe image into a user-mode binary breaks its load.
        bool user_mode_export = false;
        if (!type.import_library.empty())
        {
            if (type.ns.starts_with("Windows.Win32") && !type.ns.starts_with("Windows.Win32.Kernel"))
                user_mode_export = true;
            else
            {
                std::string where = probe_sdk_lib_dir(type.import_library);
                user_mode_export = (where == "um" || where == "ucrt");
            }
        }
        // UCRT <stdio.h> inline-only family. printf/fprintf/scanf/fscanf/
        // sscanf exist ONLY as static inline bodies in the real headers —
        // they forward to __imp___stdio_common_vfprintf/_vfscanf. NO SDK
        // import library exports these five names (verified by dumpbin
        // /linkermember across all 550 um/ucrt/km libs: the only provider is
        // the static /MT libucrt.lib, which the /MD build this driver links
        // never pulls) — a link probe of all 78 ucrtbase-attributed names
        // under the cl /MD default-lib set resolves every one EXCEPT these
        // five (LNK2019). So the ucrt alias alone would trade a silent stub
        // for a guaranteed link error: gate them back to the intentional
        // stub, the same truthful fallback as header-only kernel rows.
        static const std::set<std::string> ucrt_stdio_inline_only = {
            "printf", "fprintf", "scanf", "fscanf", "sscanf",
        };
        if (user_mode_export && ucrt_stdio_inline_only.count(fname))
        {
            std::string stem = type.import_library;
            auto dot = stem.rfind('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);
            for (auto& c : stem) c = (char)std::tolower((unsigned char)c);
            if (stem == "ucrtbase" || stem == "ucrt") user_mode_export = false;
        }
        // strsafe.h defines its whole public surface as `__inline`
        // (STRSAFEAPI); win32metadata's per-row ImportLibrary scatters
        // accidental static-lib providers. Row-local probe of all 35
        // extern rows (dumpbin /linkermember of each row's OWN
        // attribution lib): 22 rows' libs do not archive the plain C
        // symbol — StringCchCopyA is attributed Htmlhelp.Lib, whose only
        // copy is C++-mangled (?StringCchCopyA@@YAJPEAD_KPEBD@Z), which
        // cannot satisfy an extern "C" reference (linked with
        // htmlhelp.lib on the line: LNK2019). Those always fail → the
        // intentional stub, same rule as the ucrt inline-only family.
        // The other 13 rows (e.g. StringCchCopyW -> muiload.lib, which
        // really archives the symbol) keep their externs.
        if (type.ns == "Windows.Wdk.STRSAFE")
        {
            static const std::set<std::string> strsafe_no_link = {
                "StringLengthWorkerW", "StringValidateDestW", "StringCopyWorkerW",
                "StringVPrintfWorkerW", "StringCbCopyW", "StringCchPrintfW",
                "StringCbPrintfW", "StringCchLengthW", "StringValidateDestA",
                "StringCopyWorkerA", "StringCchCopyA", "StringLengthWorkerA",
                "StringValidateDestAndLengthA", "StringVPrintfWorkerA",
                "StringCchCatA", "StringCchVPrintfA", "StringCchPrintfA",
                "StringCchLengthA", "StringCchVPrintfW", "StringValidateDestAndLengthW",
                "StringCchCatW", "StringCbLengthW",
            };
            if (strsafe_no_link.count(fname)) user_mode_export = false;
        }
        if (user_mode_export)
        {
            std::string decl_cc = "__stdcall";
            switch (type.calling_convention) { case 1: decl_cc = "__cdecl"; break; case 3: decl_cc = "__thiscall"; break; case 4: decl_cc = "__fastcall"; break; }
            // CRT row: alias the std-visible declaration (see
            // crt_global_alias) instead of redeclaring it extern "C".
            if (crt_global_alias(fname))
                w.write("export using ::%;\n", fname);
            else
                w.write("export extern \"C\" % % %(%);\n", ret, decl_cc, fname, params);
            w.write("// Import library: %\n", type.import_library);
            if (type.last_error) w.write("// SetLastError: true\n");
            w.write("\n");
        }
        else
        {
            if (cc.empty())
                w.write("export inline % %(%) {\n", ret, fname, params);
            else
                w.write("export inline % % %(%)\n{\n", ret, cc, fname, params);
            if (ret == "void")
                w.write("    // %\n}\n\n", lib_name);
            else
                w.write("    // %\n    return {};\n}\n\n", lib_name);
        }
    }
}

static void write_wdk_ioctl_constant(writer& w, std::string const& name, std::uint32_t code)
{
    std::uint32_t dt = (code >> 16) & 0xFFFF;
    std::uint32_t fc = (code >> 2) & 0xFFF;
    w.write("inline constexpr std::uint32_t % = 0x%08X;  // DeviceType=0x%04X, Func=0x%03X\n", name, code, dt, fc);
}

// The CRT argv/argc are intentionally unused: the raw command line is
// re-split below (the CRT mangles quoted paths ending in a backslash).
// compile/copy helpers shared by -winui and -compile (defined after run)
int compile_project_folder(std::filesystem::path const& proj_folder, std::string const& app_name, std::string const& final_out = {});    void copy_winui_pri();
    void copy_winui_assets();
    void copy_winui_bootstrap_dll();

export int run(int const, char**)
{
    // Unbuffered stdout so verbose phase progress is visible live when the
    // output is redirected (generation + compile runs take minutes).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int result = 0;
    try
    {
        // Re-split the raw command line ourselves: the CRT's argv parsing
        // mangles quoted paths that end in a backslash ("C:\dir\"), so
        // accept the arguments exactly as the user typed them instead.
        // GetCommandLineW always starts with the exe name — drop it.
        std::vector<std::string> tokens = split_command_line();
        if (!tokens.empty())
        {
            tokens.erase(tokens.begin());
        }

        // No arguments: print the help text and exit, mirroring winversec's
        // argc<=1 behaviour — an explicit option is required to run a
        // generation or compile pass, never an implicit full run.
        if (tokens.empty())
        {
            print_usage();
            return 0;
        }

        arg_parser args{ tokens };

        if (args.exists("help") || args.exists("?"))
        {
            print_usage();
            return 0;
        }

        // `-std` alone builds the std named module and stops — it never
        // enters the generation path, so an absent -input can never run
        // the in-place stale-module cleanup against the wrong folder
        if (args.exists("std") && !args.exists("input"))
        {
            std::filesystem::path std_out = args.value("std");
            if (std_out.empty())
            {
                std_out = args.value("output", ".");
            }
            return build_std_module(std_out) ? 0 : 1;
        }

        // -target exe,sys,dll — explicit link targets (comma-separated,
        // case-insensitive). Parsed here, before any build path, so the
        // quick-test `-compile <folder>` flow sees it too (process_args does
        // not run on that path). Empty = auto-detect from project sources.
        for (auto const& spec : args.values("target"))
        {
            std::string item;
            auto flush = [&]()
            {
                if (item.empty()) return;
                for (char& c : item)
                    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                if (item != "exe" && item != "sys" && item != "dll")
                    throw std::runtime_error("invalid -target '" + item + "' (expected exe, sys, dll)");
                settings.targets.insert(item);
                item.clear();
            };
            for (char c : spec)
            {
                if (c == ',') flush();
                else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') item.push_back(c);
            }
            flush();
        }
        // -target only makes sense for the -compile link phase: with -winui
        // (mutually exclusive with -compile) or generation-only runs there is
        // no requested link step to gate.
        if (!settings.targets.empty() && !args.exists("compile"))
        {
            throw std::runtime_error("-target requires -compile (targets select the exe/sys/dll link steps of the compile phase)");
        }
        // Final-output path switches: with -input, `-compile <path>` (and
        // `-winui [<path>]` / `-winui_no_compile [<path>]`) name the folder
        // that receives ONLY the final linked binaries; generated modules
        // and build intermediates stay with -output.
        if (args.exists("winui") || args.exists("winui_no_compile") ||
            (args.exists("compile") && args.exists("input")))
        {
            std::string key = args.exists("winui") ? "winui"
                : args.exists("winui_no_compile") ? "winui_no_compile" : "compile";
            std::string p = args.value(key);
            if (!p.empty()) settings.final_output = p;
        }

        // `-compile <folder>` alone: compile an existing projection folder
        // in place — modules, app .cpp sources, then link <name>.exe (or
        // <name>.sys when a driver source is present). Quick-test path:
        // no regeneration, no -input needed.
        if (args.exists("compile") && !args.exists("input")
            && !args.exists("winui") && !args.exists("winui_no_compile"))
        {
            std::filesystem::path folder = args.value("compile");
            if (folder.empty() || !std::filesystem::is_directory(folder))
            {
                std::printf("cppwinc : error -compile needs a projection folder: cppwinc -compile <folder>\n");
                return 1;
            }
            // Quick-test: <folder> is the SOURCE projection. When -output is
            // given it receives ONLY the final binaries (exe/sys/dll);
            // otherwise they land in place next to the sources.
            std::string final_out = args.value("output");
            return compile_project_folder(std::filesystem::weakly_canonical(folder), {}, final_out);
        }

        if (!args.exists("input") && !args.exists("winui") && !args.exists("winui_no_compile"))
        {
            print_usage();
            return 0;
        }

        process_args(args);

        if (settings.verbose)
        {
            std::printf("%s v%s\n", std::string(cppwinc::strings::product_name).c_str(), std::string(cppwinc::strings::version_string).c_str());
            for (auto const& file : settings.input)
            {
                std::printf(" in:  %s\n", file.string().c_str());
            }
            std::printf(" out: %s\n", settings.output_folder.c_str());
        }

        auto phase_t0 = std::chrono::steady_clock::now();
        auto phase = [&](char const* what)
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phase_t0).count();
            std::printf("-- phase [%7.2fs] %s\n", ms / 1000.0, what);
        };

        winmd::metadata metadata{ settings.input };
        phase("metadata loaded");

        std::filesystem::path output_folder(settings.output_folder);

        // Track every module file emitted by this run. All writes go through
        // write_output_file(), which is content-aware: an existing file with
        // identical content is left untouched (mtime preserved) so incremental
        // compiles only recompile modules that actually changed, and stale
        // modules from earlier input sets are never rewritten.
        std::mutex write_mutex;
        std::set<std::string, std::less<>> emitted_this_run;
        // namespace -> owning module name (SCC group). Filled after the
        // grouping phase; declared before write_output_file so every module
        // write can remap per-namespace Win32 imports to their owner.
        std::map<std::string, std::string, std::less<>> ns_owner;
        auto write_output_file = [&](std::filesystem::path const& path, std::string const& content)
        {
            {
                std::lock_guard<std::mutex> lock(write_mutex);
                emitted_this_run.insert(path.stem().string());
            }
            // Full-mode SCC grouping merges related Win32 namespaces into one
            // owner module, so `import Windows.Win32.X;` emitted by the
            // handwritten templates (winrt_base, the App entry, view modules)
            // must be remapped to X's owner or the import fails with C2230.
            // Generated Win32 modules already name their owner and are a
            // no-op here; WDK (Windows.Wdk.*) and winrt.* lines are untouched.
            std::string text = content;
            if (!ns_owner.empty() && text.find("import Windows.Win32") != std::string::npos)
            {
                std::string out;
                std::set<std::string, std::less<>> seen;
                size_t pos = 0;
                while (pos < text.size())
                {
                    size_t eol = text.find('\n', pos);
                    if (eol == std::string::npos) eol = text.size();
                    std::string_view lv(text.data() + pos, eol - pos);
                    if (!lv.empty() && lv.back() == '\r') lv.remove_suffix(1);
                    std::string line;
                    bool dropped = false;
                    if (lv.starts_with("import ") && lv.ends_with(";"))
                    {
                        std::string mod(lv.substr(7, lv.size() - 8));
                        if (mod == "Windows.Win32" || mod.starts_with("Windows.Win32."))
                        {
                            auto oit = ns_owner.find(mod);
                            std::string resolved = (oit != ns_owner.end() && !oit->second.empty())
                                ? sanitize_module_name(oit->second) : mod;
                            // Several source namespaces can share one owner;
                            // one import per resolved module.
                            if (!seen.insert(resolved).second)
                            {
                                dropped = true;
                            }
                            else
                            {
                                line = "import " + resolved + ";";
                            }
                        }
                    }
                    if (!dropped)
                    {
                        if (line.empty()) line.assign(text, pos, eol - pos);
                        out += line;
                        out += '\n';
                    }
                    pos = eol + 1;
                }
                text = std::move(out);
            }
            if (std::filesystem::exists(path))
            {
                std::ifstream existing(path, std::ios::binary);
                std::string old{ std::istreambuf_iterator<char>(existing), std::istreambuf_iterator<char>() };
                if (old == text)
                {
                    return;
                }
            }
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary);
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
        };

        // collect projected types per namespace
        type_mapper mapper{ metadata };
        std::vector<winmd::type_info const*> component_classes;

        // pass A: collect namespaces that get modules, their dependencies,
        // and any component classes. Auto-detect Win32/WDK namespaces.
        // The scan covers every namespace of every loaded winmd (the whole
        // SDK projection for -winui projects), so it runs in parallel; the
        // results are merged back in namespace-map order so downstream
        // grouping/emission is deterministic and identical to a serial pass.
        std::vector<std::string> module_ns;
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>> ns_deps;
        std::set<std::string, std::less<>> win32_namespaces;
        std::set<std::string, std::less<>> wdk_namespaces;
        {
            // Snapshot namespaces in map order (stable across threads).
            std::vector<std::pair<std::string, winmd::namespace_info const*>> ns_order;
            ns_order.reserve(metadata.namespaces().size());
            for (auto const& [ns, info] : metadata.namespaces())
                ns_order.emplace_back(ns, &info);

            // Umbrella-prefix sanity check. Entries are raw string prefixes
            // (see the filter in ns_worker below), so a single root name in
            // the list ("Windows", which prefix-matches Windows.Wdk, Windows.UI,
            // ... every namespace) silently swallows the whole list and the
            // run emits all modules — indistinguishable from a broken filter.
            // Count each entry's namespace hits and shout when one looks like
            // an umbrella covering a majority of the tree while the caller
            // clearly asked for a scoped subset.
            if (!settings.module_include.empty())
            {
                std::size_t total_ns = ns_order.size();
                for (auto const& inc : settings.module_include)
                {
                    std::size_t hits = 0;
                    for (auto const& [ns, info] : ns_order)
                    {
                        (void)info;
                        if (ns.starts_with(inc)) ++hits;
                    }
                    if (settings.module_include.size() > 1 && hits * 2 > total_ns)
                    {
                        std::printf("cppwinc : warning: -module_include entry '%s' matches %zu of %zu namespaces "
                                    "(prefix match) and swallows the rest of the list; use the exact "
                                    "namespace(s) you meant\n",
                                    inc.c_str(), hits, total_ns);
                    }
                }
            }

            // -module_include entries are namespace PREFIXES, matching -include/
            // -exclude (matches() uses starts_with) and the help examples
            // (-module_include Windows.Win32.Graphics selects the Graphics
            // subtree). A prefix that names a real namespace therefore also
            // selects its children, and because WDF-style families are flat
            // siblings (Windows.Wdk.Wdf / WdfDevice / ...) a plain string
            // prefix selects the whole family. The self-consistency fixpoint
            // below adds whatever the selected namespaces reference, so a
            // subtree filter never emits dangling refs.

            struct ns_acc
            {
                std::set<std::string, std::less<>> selected;      // projected namespaces
                std::map<std::string, std::set<std::string, std::less<>>, std::less<>> deps;
                std::set<std::string, std::less<>> win32, wdk;
                // component classes keyed by namespace so the final merge can
                // restore the original namespace-map order (deterministic)
                std::map<std::string, std::vector<winmd::type_info const*>, std::less<>> comps_by_ns;
            };

            std::size_t ncores = system_core_count();
            std::size_t nw = std::min(ncores, ns_order.size());
            std::vector<ns_acc> accs(nw);
            std::atomic<std::size_t> next{ 0 };
            // owner[i] = accumulator index that processed ns_order[i] (-1 = skipped)
            std::vector<int> owner(ns_order.size(), -1);
            auto ns_worker = [&](std::size_t t)
            {
                auto& acc = accs[t];
                while (true)
                {
                    std::size_t i = next.fetch_add(1);
                    if (i >= ns_order.size()) break;
                    std::string const& ns = ns_order[i].first;
                    auto const& info = *ns_order[i].second;
                    if (std::getenv("CPPWINC_NS_TRACE") && (ns.find("ROAPI") != std::string::npos || ns.find("ERRHANDLING") != std::string::npos || ns.find("AppSDK") != std::string::npos))
                    {
                        std::fprintf(stderr, "[ns] %s classes=%zu ifaces=%zu enums=%zu structs=%zu dels=%zu\n",
                            ns.c_str(), info.classes.size(), info.interfaces.size(), info.enums.size(),
                            info.structs.size(), info.delegates.size());
                    }
                    if (ns == "Windows.Foundation.Metadata")
                    {
                        continue;
                    }
                    auto model = classify_namespace(ns);
                    if (model == ns_projection_model::win32)
                        acc.win32.insert(ns);
                    else if (model == ns_projection_model::wdk)
                        acc.wdk.insert(ns);

                    bool projected = false;
                    for (auto const* t : info.classes)
                    {
                        if (std::getenv("CPPWINC_NS_TRACE") && (ns.find("ROAPI") != std::string::npos || ns.find("ERRHANDLING") != std::string::npos || ns.find("AppSDK") != std::string::npos))
                        {
                            std::fprintf(stderr, "[ns-type] %s: %s internal=%d api_contract=%d in_projection=%d methods=%zu\n",
                                ns.c_str(), t->name.c_str(), (int)t->internal_, (int)t->api_contract,
                                (int)in_projection(*t), t->methods.size());
                        }
                        if (settings.component && is_component_type(*t))
                        {
                            acc.comps_by_ns[ns].push_back(t);
                            projected = true;
                            continue;
                        }
                        if (in_projection(*t))
                        {
                            projected = true;
                        }
                    }
                    for (auto const* t : info.interfaces) projected = projected || in_projection(*t);
                    for (auto const* t : info.enums) projected = projected || in_projection(*t);
                    for (auto const* t : info.structs) projected = projected || in_projection(*t);
                    for (auto const* t : info.delegates) projected = projected || in_projection(*t);

                    if (!projected)
                    {
                        continue;
                    }
                    if (!settings.module_include.empty())
                    {
                        // Plain string prefix, matching matches() semantics
                        // used by -include/-exclude. A dot-boundary rule was
                        // wrong here: the WDF namespaces are flat siblings
                        // (Windows.Wdk.Wdf, Windows.Wdk.WdfDevice, ...), so
                        // "Windows.Wdk.Wdf" must select the whole Wdf* family
                        // but a dot check rejected WdfDevice (the next char is
                        // 'D', not '.') and yielded only the single leaf.
                        bool included = false;
                        for (auto const& inc : settings.module_include)
                        {
                            if (ns.starts_with(inc))
                            {
                                included = true;
                                break;
                            }
                        }
                        if (!included)
                        {
                            continue;
                        }
                    }
                    if (settings.module_exclude.contains(ns))
                    {
                        continue;
                    }
                    acc.selected.insert(ns);
                    collect_namespace_deps(info, metadata, acc.deps[ns]);
                    owner[i] = static_cast<int>(t);
                }
            };
            if (nw > 1)
            {
                std::vector<std::jthread> ws;
                ws.reserve(nw);
                for (std::size_t t = 0; t < nw; ++t)
                    ws.emplace_back(ns_worker, t);
                for (auto& w : ws) if (w.joinable()) w.join();
            }
            else
            {
                ns_worker(0);
            }

            // Merge in namespace-map order (deterministic output).
            for (std::size_t i = 0; i < ns_order.size(); ++i)
            {
                int t = owner[i];
                std::string const& ns = ns_order[i].first;
                if (t >= 0)
                {
                    auto& acc = accs[static_cast<std::size_t>(t)];
                    if (acc.selected.contains(ns))
                        module_ns.push_back(ns);
                    auto dit = acc.deps.find(ns);
                    if (dit != acc.deps.end())
                        ns_deps[ns] = std::move(dit->second);
                }
                // component classes are collected per namespace (any owner)
                for (auto& acc : accs)
                {
                    auto cit = acc.comps_by_ns.find(ns);
                    if (cit != acc.comps_by_ns.end())
                        component_classes.insert(component_classes.end(), cit->second.begin(), cit->second.end());
                }
            }
            for (auto& acc : accs)
            {
                win32_namespaces.insert(acc.win32.begin(), acc.win32.end());
                wdk_namespaces.insert(acc.wdk.begin(), acc.wdk.end());
            }

            // -module_include / -include must be self-consistent. A selected
            // namespace's signatures reference types owned by other modules,
            // and an "export import" line is emitted only for a module that
            // was actually generated — so a filter that leaves those owners
            // out emits the reference with no import, which is guaranteed-
            // broken output (C2039 'tagDEC is not a member of global
            // namespace', C2230 'could not find module'). Pull the transitive
            // dependency closure of the selected set into the selection.
            // (Both filters need this: -include prunes selection by namespace
            // prefix before this point, so without the closure a Gdi-only
            // include never generated WINDEF — the module of a struct field's
            // type — and failed C2039 on POINTL.) Full runs select everything
            // already, so this is a no-op there, and -winui's type-level
            // closure runs afterwards and still prunes to what the app needs.
            // Namespaces the user excluded stay out.
            if (!settings.module_include.empty() || !settings.include.empty())
            {
                std::set<std::string, std::less<>> selected(module_ns.begin(), module_ns.end());
                std::vector<std::string> queue(module_ns.begin(), module_ns.end());
                while (!queue.empty())
                {
                    std::string ns = std::move(queue.back());
                    queue.pop_back();
                    auto nit = metadata.namespaces().find(ns);
                    if (nit == metadata.namespaces().end()) continue;
                    auto dit = ns_deps.find(ns);
                    if (dit == ns_deps.end())
                    {
                        collect_namespace_deps(nit->second, metadata, ns_deps[ns]);
                        dit = ns_deps.find(ns);
                    }
                    for (auto const& d : dit->second)
                    {
                        if (d == ns || selected.contains(d)) continue;
                        if (d == "Windows.Foundation.Metadata") continue;
                        if (settings.module_exclude.contains(d)) continue;
                        if (!metadata.namespaces().contains(d)) continue;
                        selected.insert(d);
                        // Types are pruned per-type by the raw -include
                        // prefixes (in_projection); register the pulled
                        // namespace so its types survive too.
                        g_closure_namespaces.insert(d);
                        queue.push_back(d);
                    }
                }
                if (selected.size() != module_ns.size())
                {
                    // Rebuild in namespace-map order so downstream grouping
                    // and emission stay deterministic.
                    module_ns.clear();
                    for (auto const& [n, info] : ns_order)
                    {
                        if (selected.contains(n)) module_ns.push_back(n);
                    }
                }
            }
        }

        phase("pass A done");

        // group namespaces into modules: merge strongly connected
        // components (namespace-level first, then module-level) until
        // the module dependency graph is acyclic
        std::set<std::string, std::less<>> in_nodes(module_ns.begin(), module_ns.end());
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>> groups;
        {
            scc_finder finder{ ns_deps, in_nodes };
            for (auto const& n : module_ns)
            {
                if (!finder.index.contains(n))
                {
                    finder.visit(n);
                }
            }
            for (auto const& scc : finder.result)
            {
                if (scc.size() == 1)
                {
                    groups[scc[0]].insert(scc[0]);
                }
                else
                {
                    std::string owner = common_ns_prefix(scc);
                    if (owner.empty())
                    {
                        owner = scc[0];
                    }
                    for (auto const& n : scc)
                    {
                        groups[owner].insert(n);
                    }
                }
            }
        }
        for (;;)
        {
            std::map<std::string, std::string, std::less<>> owner_of;
            for (auto const& [owner, members] : groups)
            {
                for (auto const& m : members)
                {
                    owner_of[m] = owner;
                }
            }
            if (settings.verbose)
            {
                auto git = groups.find("Windows.Win32");
                if (git != groups.end())
                    std::printf("  [trace] ns-level group 'Windows.Win32' members=%zu\n", git->second.size());
            }
            for (auto const& ns : module_ns)
            {
                if (groups.contains(ns))
                {
                    groups[ns].insert(ns);
                    owner_of[ns] = ns;
                }
            }

            // module-level dependency graph
            std::map<std::string, std::set<std::string, std::less<>>, std::less<>> mod_deps;
            for (auto const& [owner, members] : groups)
            {
                for (auto const& m : members)
                {
                    auto it = ns_deps.find(m);
                    if (it == ns_deps.end())
                    {
                        continue;
                    }
                    for (auto const& d : it->second)
                    {
                        if (in_nodes.contains(d))
                        {
                            mod_deps[owner].insert(owner_of[d]);
                        }
                    }
                }
                mod_deps[owner].erase(owner);
            }
            std::set<std::string, std::less<>> owners;
            for (auto const& [o, members] : groups)
            {
                (void)members;
                owners.insert(o);
            }
            scc_finder mf{ mod_deps, owners };
            for (auto const& o : owners)
            {
                if (!mf.index.contains(o))
                {
                    mf.visit(o);
                }
            }

            bool merged_any = false;
            std::map<std::string, std::set<std::string, std::less<>>, std::less<>> next;
            for (auto const& scc : mf.result)
            {
                if (scc.size() == 1)
                {
                    next[scc[0]] = groups[scc[0]];
                }
                else
                {
                    merged_any = true;
                    std::vector<std::string> all_ns;
                    for (auto const& o : scc)
                    {
                        for (auto const& m : groups[o])
                        {
                            all_ns.push_back(m);
                        }
                    }
                    std::string owner = common_ns_prefix(all_ns);
                    if (owner.empty())
                    {
                        owner = scc[0];
                    }
                    for (auto const& n : all_ns)
                    {
                        next[owner].insert(n);
                    }
                }
            }
            groups = std::move(next);
            if (!merged_any)
            {
                break;
            }
        }

        phase("grouping done");

        // namespace -> owning module name (dotted)
        // Win32/WDK namespaces use their namespace as the module name
        for (auto const& [owner, members] : groups)
        {
            for (auto const& m : members)
            {
                ns_owner[m] = owner;
            }
        }

        phase("ns_owner done");

        // base module (the runtime template carries its own module header).
        // Emitted AFTER grouping so the import remap below can rewrite the
        // per-namespace Win32 imports to their SCC-owner modules.
        if (settings.base)
        {
            std::string base_ixx(cppwinc::strings::base_module_ixx);
            replace_all(base_ixx, "@STL_GMF@", "");
            replace_all(base_ixx, "@STD_IMPORT@", cppwinc::strings::std_import);
            writer base;
            base.write("%", base_ixx);
            write_output_file(output_folder / "winrt_base.ixx", base.str());
        }

        // Track which classes have view modules (to skip component struct generation)
        std::set<std::string, std::less<>> view_class_names;

        // View class names for the generator: app-component instantiations
        // inside a shell body must route to the page module's get_<Page>()
        // accessor instead of a bare constructor call.
        winuic::g_view_type_names.clear();

        // Populate g_type_ns and g_project_nss from metadata
        // so is_app_or_unknown_type() correctly identifies component vs platform types.
        // The Windows SDK winmd carries UWP-era Windows.UI.Xaml.* duplicates of the
        // WinUI3 Microsoft.UI.Xaml.* types (ColorPicker, ToggleSwitch, Popup, ...); a
        // Windows.UI.Xaml entry must NEVER overwrite the Microsoft.UI.Xaml one or the
        // projection emits winrt::Windows.UI.Xaml... names that do not exist in WinUI3.
        for (auto const& [ns, info] : metadata.namespaces())
        {
            bool is_uwp_xaml = ns.rfind("Windows.UI.Xaml", 0) == 0;
            // Win32/WDK namespaces (winversec-projected) must never feed the
            // XAML name map: names like Image/Frame/Panel also exist there
            // (different types), and a Win32 hit would make the view codegen
            // emit winrt::Windows::Win32::... for XAML elements.
            if (ns.rfind("Windows.Win32", 0) == 0 || ns.rfind("Windows.Wdk", 0) == 0) continue;
            for (auto const* t : info.classes)
            {
                if (is_uwp_xaml && winuic::g_type_ns.count(t->name)) continue;
                winuic::g_type_ns[t->name] = ns;
            }
            for (auto const* t : info.interfaces)
            {
                if (is_uwp_xaml && winuic::g_type_ns.count(t->name)) continue;
                winuic::g_type_ns[t->name] = ns;
            }
        }
        // Mark component namespaces (from the app's winmd)
        if (settings.component)
        {
            for (auto const& [ns, info] : metadata.namespaces())
            {
                bool has_component = false;
                for (auto const* t : info.classes)
                {
                    if (is_component_type(*t)) { has_component = true; break; }
                }
                if (has_component) winuic::g_project_nss.insert(ns);
            }
        }

        phase("globals done");
        // winuic view generation (before projection, so we can
        // scan imports and only generate needed modules)
        {
            winuic::attribute_catalog catalog;
            if (!metadata.module_catalog_json().empty())
            {
                catalog.load_from_json(metadata.module_catalog_json());
            }
            // Pre-scan: find the app's root window class - any class with a
            // Window-derived base type. The host layer must not hardcode a
            // class name (App.xaml/MainWindow.xaml style coupling); whatever
            // the project named its window, App::Start constructs that class.
            bool any_main_window = false;
            std::string main_window_class;
            std::string main_window_ns;
            for (auto const& [ns, info] : metadata.namespaces())
                for (auto const* t : info.classes)
                    if (t->base && t->base->name == "Window")
                    {
                        if (!any_main_window || t->name == "MainWindow" || t->name == "MainView")
                        {
                            any_main_window = true;
                            main_window_class = t->name;
                            main_window_ns = t->ns.empty() ? "App" : t->ns;
                            if (t->name == "MainWindow") goto found_main_window;
                        }
                    }
        found_main_window:;
            // Collect all view generation jobs first, then emit in parallel (all cores)
            struct view_job {
                std::string mod_ns;
                std::string view_name;
                std::string json;
                bool dark_titlebar;
                bool app_icon;
                bool dark_context_menu;
                bool extend_content_into_titlebar;
                bool has_main_window;
                std::string main_window_class;
                std::string main_window_ns;
                std::string base_type;
                std::string app_icon_path;
                std::string backdrop_type;
                std::string titlebar_text;
                bool shell_window = false;
                // Sibling view modules this view must import (get_<Name>() deps).
                std::vector<std::string> component_view_imports;
                // Events attached to component-view instances inside this view
                // (component, event) - drives add_<E> forwarding on the component.
                std::vector<std::pair<std::string, std::string>> component_view_events;
                // Events other views attached to THIS view's instances
                // (driver-aggregated; emitted as forwarding accessors).
                std::vector<std::string> exposed_component_events;
            };
            std::vector<view_job> view_jobs;
            // Names of every type the loaded metadata actually exports. The view
            // generator gates its fallback stubs on this set - a stub for a type
            // that IS projected (e.g. ControlExample in the app winmd) would be a
            // second definition and break every importing module (C1117).
            std::set<std::string> projected_names;
            for (auto const& [pns, pinfo] : metadata.namespaces())
            {
                for (auto const* t : pinfo.classes) projected_names.insert(pns + "." + t->name);
                for (auto const* t : pinfo.interfaces) projected_names.insert(pns + "." + t->name);
                for (auto const* t : pinfo.structs) projected_names.insert(pns + "." + t->name);
            }
            for (auto const& [ns, info] : metadata.namespaces())
            {
                for (auto const* t : info.classes)
                {
                    if (t->winuic_json.empty()) continue;
                    view_class_names.insert(t->name);
                    view_job vj;
                    vj.view_name = t->name;
                    vj.mod_ns = t->ns.empty() ? "App" : t->ns;
                    vj.json = t->winuic_json;
                    vj.dark_titlebar = true;
                    vj.app_icon = t->app_icon || !metadata.module_app_icon_path().empty();
                    vj.dark_context_menu = t->dark_context_menu || metadata.module_dark_context_menu();
                    vj.extend_content_into_titlebar = metadata.module_extend_content_into_titlebar();
                    vj.app_icon_path = metadata.module_app_icon_path();
                    vj.backdrop_type = metadata.module_backdrop_type();
                    vj.titlebar_text = metadata.module_titlebar_text();
                    vj.has_main_window = any_main_window;
                    vj.main_window_class = main_window_class;
                    vj.main_window_ns = main_window_ns;
                    if (t->base)
                    {
                        if (t->base->name == "Application") vj.base_type = "Application";
                        else if (t->base->name == "Window") vj.base_type = "Window";
                        else if (t->base->name == "UserControl") vj.base_type = "UserControl";
                        else vj.base_type = t->base->name;  // real framework base (Button, ...)
                    }                    else vj.base_type = "UserControl";
                    view_jobs.push_back(std::move(vj));
                }
            }
            for (auto const& n : view_class_names) winuic::g_view_type_names.insert(n);
            // A view may carry the whole shell (nav host + pages) inside the
            // Application view itself (winui syntax: `export view App` with
            // .titlebar/.darkTitlebar/.backdrop chrome and a body). Nothing
            // derives from Window then, and the host would activate an empty
            // default window. Promote that view to the app shell: the same
            // module keeps the AppHost machinery and additionally emits the
            // window class (AppView) + launch_App() from the App view's own
            // body. One source view -> one module, no phantom types.
            if (!any_main_window)
            {
                for (auto& vj : view_jobs)
                    if (vj.base_type == "Application")
                    {
                        vj.shell_window = true;
                        any_main_window = true;
                        main_window_class = vj.view_name;
                        main_window_ns = vj.mod_ns;
                    }
                for (auto& vj : view_jobs)
                {
                    vj.has_main_window = any_main_window;
                    vj.main_window_class = main_window_class;
                    vj.main_window_ns = main_window_ns;
                }
            }
            // Navigation map: NavigationView tags (raw "Tag" attrs in the
// window view's layout JSON, in menu order) -> page runtime class
            // names. Tag->page resolution is name-normalized (home -> HomePage),
            // never hardcoded: whatever the project names its pages, the
            // projection emits a Frame::Navigate registry for them. The .winmd
            // stays language-agnostic - this map only shapes the emitted C++.
            std::vector<std::pair<std::string, std::string>> nav_pages;
            {
                std::function<void(winuic::layout_node const&, std::vector<std::string>&)> collect_tags =
                    [&](winuic::layout_node const& n, std::vector<std::string>& out)
                {
                    for (auto const& [k, v] : n.raw_attrs)
                    {
                        if (k == "Tag" || k == "tag") out.push_back(v);
                        else if (n.control == "NavigationViewItem" && (k == "x:Name" || k == "Name" || k == "Content"))
                            out.push_back(v);
                    }
                    for (auto const& ch : n.children) collect_tags(ch, out);
                };
                for (auto const& vj : view_jobs)
                {
                    if (vj.view_name != main_window_class) continue;
                    winuic::layout_view lv = winuic::parse_layout_json(vj.json, vj.view_name);
                    std::vector<std::string> tags;
                    for (auto const& rn : lv.root) collect_tags(rn, tags);
                    auto norm = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
                    for (auto const& tag : tags)
                    {
                        bool dup = false;
                        for (auto const& [t, p] : nav_pages) if (t == tag) { dup = true; break; }
                        if (dup) continue;
                        for (auto const& pj : view_jobs)
                        {
                            if (pj.view_name == main_window_class) continue;
                            std::string nm = norm(pj.view_name);
                            if (nm == norm(tag) || nm == norm(tag) + "page")
                            {
                                nav_pages.emplace_back(tag, pj.mod_ns + "." + pj.view_name);
                                break;
                            }
                        }
                    }
                    // Ensure Home item maps to HomePage when present
                    {
                        bool have_home_tag = false;
                        for (auto const& [t, p] : nav_pages) if (norm(t) == "home") { have_home_tag = true; break; }
                        if (!have_home_tag)
                        {
                            for (auto const& pj : view_jobs)
                            {
                                if (pj.view_name == main_window_class) continue;
                                if (norm(pj.view_name) == "homepage")
                                {
                                    nav_pages.emplace_back("Home", pj.mod_ns + "." + pj.view_name);
                                    break;
                                }
                            }
                        }
                    }
                    // NavigationView's built-in settings item (IsSettingsVisible)
                    // carries no Tag, so tag scanning can't see it. Wire the
                    // synthesized "__settings__" tag to a project view named
                    // SettingsPage when one exists.
                    {
                        bool have_settings_tag = false;
                        for (auto const& [t, p] : nav_pages) if (t == "__settings__") { have_settings_tag = true; break; }
                        if (!have_settings_tag)
                        {
                            for (auto const& pj : view_jobs)
                            {
                                if (pj.view_name == main_window_class) continue;
                                if (norm(pj.view_name) == "settingspage")
                                {
                                    nav_pages.emplace_back("__settings__", pj.mod_ns + "." + pj.view_name);
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            // Per-view component-view imports: a view whose body instantiates
            // another project view (ControlExample inside AcrylicPage, ...) must
            // import that sibling's module for its exported get_<Name>() -
            // otherwise the emitted accessor call has no declaration (C3861).
            // Node controls are matched against the project's own view names;
            // platform controls never collide because view names are unique.
            {
                std::function<void(winuic::layout_node const&, std::set<std::string>&)> collect_used_views =
                    [&](winuic::layout_node const& n, std::set<std::string>& out)
                {
                    out.insert(n.control);
                    for (auto const& ch : n.children) collect_used_views(ch, out);
                    for (auto const& ch : n.else_branch) collect_used_views(ch, out);
                };
                for (auto& vj : view_jobs)
                {
                    winuic::layout_view lv = winuic::parse_layout_json(vj.json, vj.view_name);
                    std::set<std::string> used;
                    for (auto const& rn : lv.root) collect_used_views(rn, used);
                    for (auto const& pj : view_jobs)
                    {
                        if (pj.view_name == vj.view_name) continue;
                        if (used.count(pj.view_name))
                            vj.component_view_imports.push_back(pj.mod_ns + "." + pj.view_name);
                    }
                    // Events attached to component-view instances: .on<E> on a
                    // node whose control is another project view. The component
                    // class must expose add_<E> forwarding for these.
                    std::function<void(winuic::layout_node const&)> collect_comp_events =
                        [&](winuic::layout_node const& n)
                    {
                        if (n.control != vj.view_name && used.count(n.control))
                            for (auto const& ev : n.events)
                                vj.component_view_events.push_back({ n.control, ev.name });
                        for (auto const& ch : n.children) collect_comp_events(ch);
                        for (auto const& ch : n.else_branch) collect_comp_events(ch);
                    };
                    for (auto const& rn : lv.root) collect_comp_events(rn);
                }
                // Aggregate per component and expose on that component's own job.
                std::map<std::string, std::set<std::string>> comp_events;
                for (auto const& vj : view_jobs)
                    for (auto const& [cn, ev] : vj.component_view_events)
                        comp_events[cn].insert(ev);
                for (auto& vj : view_jobs)
                {
                    auto it = comp_events.find(vj.view_name);
                    if (it != comp_events.end())
                        vj.exposed_component_events.assign(it->second.begin(), it->second.end());
                }
            }
            if (!view_jobs.empty()) phase("view emission start");
            // Emit view modules in parallel on all cores
            if (view_jobs.size() > 1)
            {
                std::size_t nthreads = system_core_count();
                if (nthreads > view_jobs.size()) nthreads = view_jobs.size();
                std::printf("  view emission: %zu modules, %zu threads\n", view_jobs.size(), nthreads);
                std::atomic<std::size_t> vj_idx{0};
                std::mutex dbg_m;
                std::vector<std::jthread> workers;
                workers.reserve(nthreads);
                for (std::size_t t = 0; t < nthreads; ++t)
                {
                    workers.emplace_back([&]() {
                        while (true)
                        {
                            std::size_t idx = vj_idx.fetch_add(1);
                            if (idx >= view_jobs.size()) break;
                            auto const& vj = view_jobs[idx];
                            {
                                std::lock_guard<std::mutex> lk(dbg_m);
                                std::printf("  [claim %zu/%zu %s]\n", idx + 1, view_jobs.size(), vj.view_name.c_str());
                            }
                            winuic::layout_view lv = winuic::parse_layout_json(vj.json, vj.view_name);
                            lv.namespace_name = vj.mod_ns;
                            lv.dark_titlebar = vj.dark_titlebar;
                            lv.app_icon = vj.app_icon;
                            lv.dark_context_menu = vj.dark_context_menu;
                            lv.extend_content_into_titlebar = vj.extend_content_into_titlebar;
                            lv.app_icon_path = vj.app_icon_path;
                            lv.backdrop_type = vj.backdrop_type;
                            lv.titlebar_text = vj.titlebar_text;
                            lv.has_main_window = vj.has_main_window;
                            lv.main_window_class = vj.main_window_class;
                            lv.main_window_ns = vj.main_window_ns;
                            lv.base_type = vj.base_type;
                            lv.shell_window = vj.shell_window;
                            lv.component_view_imports = vj.component_view_imports;
                            lv.exposed_component_events = vj.exposed_component_events;
                            // Only the window view hosts the nav map: pages
                            // must not import the component module (their own
                            // class name would collide with the component
                            // struct - C2872).
                            if (vj.view_name == main_window_class)
                                lv.nav_pages = nav_pages;
                            std::string code = winuic::generate_view_module(lv, catalog, metadata.namespaces().count("Windows.Foundation") > 0,
                                projected_names);
                            std::string fname = vj.mod_ns + "." + vj.view_name + ".ixx";
                            write_output_file(output_folder / fname, code);
                            {
                                std::lock_guard<std::mutex> lk(dbg_m);
                                std::printf("  [ok %zu/%zu %s]\n", idx + 1, view_jobs.size(), vj.view_name.c_str());
                            }
                        }
                    });
                }
                for (auto& w : workers)
                    if (w.joinable()) w.join();
            }
            else
            {
                for (auto const& vj : view_jobs)
                {
                    winuic::layout_view lv = winuic::parse_layout_json(vj.json, vj.view_name);
                    lv.namespace_name = vj.mod_ns;
                    lv.dark_titlebar = vj.dark_titlebar;
                    lv.app_icon = vj.app_icon;
                    lv.dark_context_menu = vj.dark_context_menu;
                    lv.extend_content_into_titlebar = vj.extend_content_into_titlebar;
                    lv.app_icon_path = vj.app_icon_path;
                    lv.backdrop_type = vj.backdrop_type;
                    lv.titlebar_text = vj.titlebar_text;
                    lv.has_main_window = vj.has_main_window;
                lv.main_window_class = vj.main_window_class;
                lv.main_window_ns = vj.main_window_ns;
                lv.base_type = vj.base_type;
                lv.shell_window = vj.shell_window;
                lv.component_view_imports = vj.component_view_imports;
                lv.exposed_component_events = vj.exposed_component_events;
                // Window view only - see the parallel-path comment above.
                if (vj.view_name == main_window_class)
                    lv.nav_pages = nav_pages;
                std::string code = winuic::generate_view_module(lv, catalog, metadata.namespaces().count("Windows.Foundation") > 0,
                    projected_names);
                std::string fname = vj.mod_ns + "." + vj.view_name + ".ixx";
                    write_output_file(output_folder / fname, code);
                }
            }
            if (!view_jobs.empty()) phase("view modules emitted");
            // main.ixx (only when WinUI views exist; re-emit on every -winui
            // run when the file already exists so the stale-module cleanup —
            // which lists "main" as generator-owned — can never delete the
            // app's bootstrap just because no view changed this run)
            if (!view_jobs.empty() || std::filesystem::exists(output_folder / "main.ixx"))
            {
                std::ostringstream entry;
                entry << "// main.ixx \u2014 Application entry point.\n";
                entry << "// generated by cppwinc from winuic-produced .winmd.\n";
                entry << "//\n";
                entry << "// The Windows App SDK bootstrapper handles dynamic dependency\n";
                entry << "// initialization itself: it prefers the OS builtin API on\n";
                entry << "// Windows 11 22H2+ and falls back to the DDLM framework\n";
                entry << "// packages on older OSes. No Win32/WinRT/WDK headers are\n";
                entry << "// included anywhere: every Win32 symbol comes from\n";
                entry << "// winversec-projected modules, every WinRT symbol from\n";
                entry << "// winrt_base.\n\n";
                entry << "module;\n\n";
                entry << "export module main;\n\n";
                entry << "import std;\n";
                entry << "import winrt_base;\n";
                // Explicit imports for every aliased namespace — no reliance
                // on DynamicDependency's transitive re-export of AppModel.
                entry << "import Windows.Win32.System.AppModel;\n";
                entry << "import Windows.Win32.System.DynamicDependency;\n";
                std::string app_ns;
                for (auto const& [ns, info] : metadata.namespaces()) {
                    for (auto const* c : info.classes) {
                        if (c->name == "App") { app_ns = ns; break; }
                    }
                    if (!app_ns.empty()) break;
                }
                if (app_ns.empty() && !settings.component_name.empty()) app_ns = settings.component_name;
                entry << "import " << app_ns << ".App;\n";
                // Local activation registry wiring: import every activatable
                // view module (the ones exporting create_<View>()) so each
                // register_local_factory_<View>() is callable from wWinMain.
                for (auto const& vj : view_jobs)
                {
                    if (vj.view_name == "App" || vj.shell_window || vj.view_name.ends_with("Window"))
                        continue; // App/window views emit launch_<X>, not create_<X>
                    entry << "import " << vj.mod_ns << "." << vj.view_name << ";\n";
                }
                entry << "\n";
                entry << "// One short alias per imported namespace, at module scope.\n";
                entry << "namespace dd = Windows::Win32::System::DynamicDependency;\n";
                entry << "namespace am = Windows::Win32::System::AppModel;\n\n";
                entry << "// extern \"C\" so the symbol keeps the plain CRT-visible name\n";
                entry << "// (MSVC module-linkage mangling would hide it from\n";
                entry << "// wWinMainCRTStartup).\n";
                entry << "extern \"C\" int __stdcall wWinMain(void* hInstance, void*, void*, int)\n";
                entry << "{\n";
                entry << "    (void)hInstance;\n";
                entry << "    // Fatal-exception self-report: any uncaught winrt/stdlib\n";
                entry << "    // exception appends its kind + HRESULT + message to\n";
                entry << "    // boot_log.txt before the process dies.\n";
                entry << "    winrt::set_crash_reporter();\n\n";
                entry << "    winrt::init_apartment();\n\n";
                entry << "    // minVersion {0,0} = any 2.x runtime; the bootstrapper resolves\n";
                entry << "    // the highest installed match for the major.minor requested.\n";
                entry << "    am::PACKAGE_VERSION min_version{};\n";
                entry << "    if (auto hr = dd::MddBootstrapInitialize2(0x00020000, nullptr, min_version,\n";
                entry << "        dd::MddBootstrapInitializeOptions::MddBootstrapInitializeOptions_None); hr < 0)\n";
                entry << "    {\n";
                entry << "        std::wcerr << L\"dynamic dependency initialization failed: 0x\"\n";
                entry << "                   << std::hex << hr << std::endl;\n";
                entry << "        return 1;\n";
                entry << "    }\n\n";
                entry << "    // Local activation registry: map every app runtime class to\n";
                entry << "    // its in-process C++ factory BEFORE App::Start. XAML control\n";
                entry << "    // instantiation may construct winrt::<ns>::<Class>() wrappers\n";
                entry << "    // during initial layout; without the registry those calls hit\n";
                entry << "    // RoActivateInstance and fail (app classes have no registry\n";
                entry << "    // entry and the exe exports nothing).\n";
                for (auto const& vj : view_jobs)
                {
                    if (vj.view_name == "App" || vj.shell_window || vj.view_name.ends_with("Window"))
                        continue;
                    entry << "    register_local_factory_" << vj.view_name << "();\n";
                }
                entry << "\n    App::Start();\n\n";
                entry << "    dd::MddBootstrapShutdown();\n";
                entry << "    return 0;\n";
                entry << "}\n";
                write_output_file(output_folder / "main.ixx", entry.str());
            }
        }

        // Auto-enable component+base when views are generated
        if (!view_class_names.empty()) {
            settings.component = true;
            settings.base = true;
            if (settings.component_name.empty())
            {
                // Component source = the app .winmd (first input_dbs entry).
                if (!settings.input_dbs.empty())
                    settings.component_name = std::filesystem::path(*settings.input_dbs.begin()).filename().replace_extension().string();
                else
                    settings.component_name = std::filesystem::path(args.values("input").front()).filename().replace_extension().string();
            }
            component_classes.clear();
            for (auto const& [ns, info] : metadata.namespaces()) {
                for (auto const* t : info.classes)
                    if (is_component_type(*t)) component_classes.push_back(t);
            }
            if (settings.verbose)
            {
                std::printf("  component: app_dbs=%zu classes=%zu\n", settings.app_dbs.size(), component_classes.size());
                for (auto const* t : component_classes)
                    std::printf("    comp: %s.%s (db=%s)\n", t->ns.c_str(), t->name.c_str(), t->db ? t->db->path().string().c_str() : "<null>");
            }
        }

        // Component diagnostics for plain -component runs. The block above
        // only reports when WinUI views were generated, so without this a
        // plain -component run prints nothing about its component selection
        // and order-independence of app_dbs cannot be verified from the
        // outside. Guarded on view_class_names so view runs still print once.
        if (settings.component && settings.verbose && view_class_names.empty())
        {
            std::printf("  component: app_dbs=%zu classes=%zu component_name=%s\n",
                settings.app_dbs.size(), component_classes.size(),
                settings.component_name.c_str());
            for (auto const* t : component_classes)
                std::printf("    comp: %s.%s (db=%s)\n", t->ns.c_str(), t->name.c_str(),
                    t->db ? t->db->path().filename().string().c_str() : "<null>");
        }

        // Find needed modules: scan view imports + compute transitive deps from metadata
        // Only active in -winui / -winui_no_compile (selective project mode):
        // a plain SDK/component projection keeps every module; only the winui
        // switches tell us which app winmd defines "the project", so only then
        // is the output pruned to the modules that project actually needs.
        std::set<std::string, std::less<>> needed_modules;
        if (settings.winui)
        {
            std::regex import_re(R"(^\s*(?:export\s+)?import\s+([\w.]+)\s*;)", std::regex::ECMAScript);
            // Step 1: scan ONLY the module files written by this run (view
            // modules, main.ixx) for their direct imports. Never scan
            // pre-existing files from earlier input sets: stale modules would
            // keep themselves (and everything they import) in the needed set
            // forever, so the whole previous dump gets regenerated each run.
            for (auto const& stem : emitted_this_run)
            {
                if (stem.starts_with("winrt.")) continue;
                // winrt_base is scanned too: its Windows.Win32.* module
                // imports (ACTIVATION, ROAPI, Foundation, ...) must seed the
                // needed set — the header-free base imports projected modules
                // instead of including COM headers. Only OS-namespace imports
                // are taken from it (skip std / internals).
                bool is_base = (stem == "winrt_base");
                auto const& p = output_folder / (stem + ".ixx");
                if (!std::filesystem::is_regular_file(p)) continue;
                std::ifstream ifs(p);
                std::string line;
                while (std::getline(ifs, line))
                {
                    std::smatch m;
                    if (std::regex_match(line, m, import_re))
                    {
                        std::string const& im = m[1].str();
                        if (is_base && !im.starts_with("Windows.") && !im.starts_with("Microsoft."))
                            continue;
                        needed_modules.insert(im);
                    }
                }
            }
            // Step 3: compute module-level transitive deps from metadata
            //   For each needed winrt.* module M:
            //     1. Find M's namespaces from groups
            //     2. For each namespace N in M, find ns_deps[N]
            //     3. Map each dep to its owning module via ns_owner
            //     4. Add that module to needed; repeat until stable
            {
                // Build module -> set of modules it depends on (from metadata)
                auto compute_mod_deps = [&](std::string const& mod) {
                    std::set<std::string, std::less<>> deps;
                    // Extract owner namespace from module name
                    std::string owner;
                    if (mod.starts_with("winrt."))
                        owner = mod.substr(6);
                    else
                        owner = mod; // Win32/WDK: module name IS the namespace

                    // Win32/WDK modules (selective -winui mode): imports come
                    // from THIS namespace's metadata deps only. Resolving
                    // through ns_owner into the SCC-merged root and expanding
                    // the root's whole member set pulled the entire SDK into
                    // every -winui build — the project only needs what the
                    // emitted module actually references.
                    if (!mod.starts_with("winrt."))
                    {
                        auto dit = ns_deps.find(owner);
                        if (dit == ns_deps.end()) return deps;
                        for (auto const& d : dit->second)
                        {
                            // Map the dep namespace to its SCC-group module:
                            // imports and emission both use group names, and
                            // per-namespace names resolve via re-export shells.
                            if (!in_nodes.contains(d)) continue;
                            auto model = classify_namespace(d);
                            if (model == ns_projection_model::win32 || model == ns_projection_model::wdk)
                                deps.insert(sanitize_module_name(ns_owner[d]));
                            else if (auto oit = ns_owner.find(d); oit != ns_owner.end() && oit->second != owner)
                                deps.insert("winrt." + oit->second);
                        }
                        return deps;
                    }

                    // WinRT modules: SCC-merged group expansion (transitive
                    // winrt.* deps; Xaml.Controls lives inside winrt.Microsoft.UI.Xaml).
                    if (auto oit = ns_owner.find(owner); oit != ns_owner.end())
                        owner = oit->second;
                    auto git = groups.find(owner);
                    if (git == groups.end()) return deps;
                    for (auto const& ns : git->second)
                    {
                        auto dit = ns_deps.find(ns);
                        if (dit == ns_deps.end()) continue;
                        for (auto const& d : dit->second)
                        {
                            auto oit = ns_owner.find(d);
                            if (oit != ns_owner.end() && oit->second != owner)
                            {
                                // Use correct module name prefix based on namespace type
                                auto model = classify_namespace(d);
                                if (model == ns_projection_model::win32 || model == ns_projection_model::wdk)
                                {
                                    // Win32/WDK: module name IS the namespace
                                    deps.insert(oit->second);
                                }
                                else
                                {
                                    // WinRT: module name has "winrt." prefix
                                    deps.insert("winrt." + oit->second);
                                }
                            }
                        }
                    }
                    return deps;
                };
                // Iteratively expand needed modules
                // In -winui mode: only expand within Microsoft.* modules
                // (winrt_base already provides Windows.Foundation types)
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    std::vector<std::string> current(needed_modules.begin(), needed_modules.end());
                    for (auto const& mod : current)
                    {
                        // A needed WinRT member module implies its owning merged
                        // module is needed too (Xaml.Controls inside
                        // winrt.Microsoft.UI.Xaml). Win32/WDK modules are emitted
                        // per-namespace in selective mode — no owner promotion,
                        // which is what dragged the merged Windows.Win32 root
                        // (and with it the whole SDK) into the needed set.
                        if (mod.starts_with("winrt."))
                        {
                            std::string bare = mod.substr(6);
                            if (auto oit = ns_owner.find(bare); oit != ns_owner.end())
                            {
                                std::string owner_mod = "winrt." + sanitize_module_name(oit->second);
                                if (needed_modules.insert(owner_mod).second)
                                    changed = true;
                            }
                        }
                        // Expand every module (including winrt.Windows.* and
                        // winversec-projected Win32/WDK): generated modules
                        // import their metadata dependencies, so each of those
                        // must exist for the module to compile. Win32 modules
                        // are NOT self-contained — DynamicDependency's functions
                        // reference PACKAGE_VERSION from AppModel, etc. The
                        // seeded Win32 set is tiny (the bootstrap's imports),
                        // so expanding their deps stays cheap.
                        auto deps = compute_mod_deps(mod);
                        if (!mod.starts_with("winrt."))
                        {
                            // Group module: ns_deps is namespace-keyed, so a
                            // group owner name finds nothing by itself. Expand
                            // every member namespace's metadata deps so the
                            // closure is complete.
                            if (auto git2 = groups.find(mod); git2 != groups.end())
                            {
                                for (auto const& m2 : git2->second)
                                {
                                    auto dit2 = ns_deps.find(m2);
                                    if (dit2 == ns_deps.end()) continue;
                                    for (auto const& d2 : dit2->second)
                                    {
                                        if (!in_nodes.contains(d2)) continue;
                                        deps.insert(d2);
                                    }
                                }
                            }
                        }
                        for (auto const& d : deps)
                        {
                            // Windows.* direct deps from Microsoft.* are OK
                            if (needed_modules.insert(d).second)
                                changed = true;
                        }
                        // A needed Win32/WDK namespace implies its SCC-group
                        // module: content lives in the group module (acyclic
                        // emission unit); the per-namespace name resolves via
                        // its re-export shell. Without this promotion the
                        // group module never enters the needed set, its shell
                        // sees no owner job and nothing is emitted.
                        if (mod.starts_with("winrt.")) continue;
                        if (auto oit = ns_owner.find(mod); oit != ns_owner.end() && oit->second != mod)
                        {
                            if (needed_modules.insert(sanitize_module_name(oit->second)).second)
                                changed = true;
                        }
                    }
                }
            }
            if (settings.verbose)
            {
                for (auto const& m : needed_modules)
                    std::printf("  needed-module: %s\n", m.c_str());
                std::size_t winrt_count = 0;
                std::size_t win32_count = 0;
                std::size_t wdk_count = 0;
                for (auto const& ns : module_ns)
                {
                    auto m = classify_namespace(ns);
                    if (m == ns_projection_model::winrt) winrt_count++;
                    else if (m == ns_projection_model::win32) win32_count++;
                    else if (m == ns_projection_model::wdk) wdk_count++;
                }
                std::printf("  needed: %zu modules (WinRT: %zu, Win32: %zu, WDK: %zu)\n",
                    needed_modules.size(), winrt_count, win32_count, wdk_count);
            }
        }

        // selective -winui type closure
        // With the needed module set known, build the TYPE-level closure:
        // only the winrt types reachable from the app's component classes
        // and from the namespaces the generated code imports. Activating
        // g_type_closure prunes in_projection(), so every downstream phase
        // (forward-decls, type emission, dep collection, P-chunk counting)
        // projects exactly the closure — not every type in each namespace.
        // Win32/WDK namespaces stay module-scoped (the needed set above).
        if (settings.winui && !component_classes.empty())
        {
            // Views instantiate a handful of platform types the app's own code
            // never names (ListViewPage/SemanticZoomPage: CollectionViewSource,
            // XamlUICommandPage: XamlUICommand). The app-code-only closure
            // prunes them, so neither the real type nor the view generator's
            // fallback stub is emitted (C2039). Seed exactly those classes.
            // Do NOT blanket-seed every view-instantiated control: exotic
            // dependency subgraphs (MediaPlayerElement -> Windows.Media.Playback)
            // blow up the huge P1 module and trip frontend ICEs (C1001).
            std::vector<winmd::type_info const*> closure_roots = component_classes;
            for (std::pair<char const*, char const*> seed : {
                    std::pair<char const*, char const*>{"Microsoft.UI.Xaml.Data", "CollectionViewSource"},
                    {"Microsoft.UI.Xaml.Input", "XamlUICommand"} })
            {
                auto nit = metadata.namespaces().find(seed.first);
                if (nit == metadata.namespaces().end()) continue;
                for (auto const* t : nit->second.classes)
                    if (t->name == seed.second)
                        closure_roots.push_back(t);
            }
            build_winui_type_closure(metadata, closure_roots, needed_modules);
            g_type_closure_active = true;

            // Recompute winrt ns_deps from the pruned projection: pass A's
            // namespace-granular deps union every type's deps in a namespace
            // (all 50K Xaml.Controls types), which is what dragged Bluetooth/
            // Calls/Email into module imports. collect_type_deps consults
            // in_projection(), now closure-aware, so only the closure's edge
            // namespaces remain.
            for (auto const& ns : module_ns)
            {
                if (classify_namespace(ns) != ns_projection_model::winrt) continue;
                auto it = metadata.namespaces().find(ns);
                if (it == metadata.namespaces().end()) continue;
                std::set<std::string, std::less<>> pruned;
                collect_namespace_deps(it->second, metadata, pruned);
                // Store RAW dep namespaces: compute_mod_deps/emit_module map
                // through ns_owner themselves (matches pass A's convention).
                ns_deps[ns] = std::move(pruned);
            }
            if (settings.verbose)
            {
                std::printf("  type closure: %zu winrt types reachable\n", g_type_closure.size());
            }
        }

        phase("needed done");

        // pass B: emit one module per group (only needed modules)
        // Win32/WDK namespaces get their own module names
        std::set<std::string, std::less<>> emitted_modules;
        struct module_job
        {
            std::string module_name;
            std::vector<std::string> members;
            std::vector<std::string> ordered;
            ns_projection_model model = ns_projection_model::winrt;
        };
        std::vector<module_job> jobs;
        // Split-time record of P-chunk membership: root namespace ->
        // { member ns -> ".P<n>" sub-name }. ns_owner maps to group owners,
        // never to .P names, so emit_module must consult this instead.
        std::map<std::string, std::map<std::string, std::string>, std::less<>> p_chunk_members;
        // Every module name this run will actually emit: each namespace's group
        // module plus each split-member re-export shell, under the same
        // needed_modules pruning the job loop below applies. Import emission
        // gates on this set (not only on emitted_modules): in FILTERED runs the
        // needed-phase appends dependency namespaces AFTER the explicit -include
        // set, so their jobs may run later than their consumers' — gating on
        // already-started jobs dropped those import edges and every consumer
        // failed C3083/C2039/C2230 on the cross-module types it referenced
        // (e.g. -include Windows.Win32.Graphics.Gdi produced a Gdi module that
        // never imported WINDEF).
        std::set<std::string, std::less<>> planned_modules;
        for (auto const& ns : module_ns)
        {
            std::string owner = ns_owner[ns];
            auto model = classify_namespace(ns);
            std::string group_name = (model == ns_projection_model::win32 || model == ns_projection_model::wdk)
                ? sanitize_module_name(owner)
                : "winrt." + owner;
            if (settings.winui)
            {
                if (!needed_modules.empty() && !needed_modules.contains(group_name)) continue;
            }
            else if (model != ns_projection_model::win32 && model != ns_projection_model::wdk)
            {
                if (!needed_modules.empty() && !needed_modules.contains(group_name)) continue;
            }
            planned_modules.insert(group_name);
            // Split members of a multi-namespace group also get a tiny
            // re-export shell module named after the namespace itself.
            std::string shell_name = sanitize_module_name(ns);
            if (shell_name != group_name) planned_modules.insert(shell_name);
        }
        for (auto const& ns : module_ns)
        {
            std::string owner = ns_owner[ns];
            auto model = classify_namespace(ns);
            // Module name: Win32/WDK use namespace directly, WinRT uses winrt. prefix
            std::string module_name;
            if (model == ns_projection_model::win32 || model == ns_projection_model::wdk)
            {
                // Win32/WDK: content always lives in the SCC-group module
                // (owner == ns for singleton groups). C++ modules forbid
                // import cycles, and Win32 namespaces interdepend heavily
                // (NSPAPI <-> WinSock <-> MINWINBASE <-> SystemServices...);
                // per-namespace modules in selective mode recreated those
                // cycles and the scheduler fell back to one giant level where
                // every consumer failed with C2230. Namespaces inside a
                // multi-member group additionally get a tiny re-export SHELL
                // module named after the namespace, so per-namespace imports
                // (winrt_base seeds, view/main imports) keep resolving.
                module_name = sanitize_module_name(owner);
            }
            else
            {
                module_name = "winrt." + owner;
            }
            // needed_modules filtering. In -winui selective mode the Win32/WDK
            // set is pruned too: only the modules the generated app code imports
            // (the bootstrap's AppModel/DynamicDependency/LibraryLoader imports)
            // are emitted. Full-mode runs keep emitting every Win32/WDK module.
            if (settings.winui)
            {
                if (!needed_modules.empty() && !needed_modules.contains(module_name)) continue;
            }
            else if (model != ns_projection_model::win32 && model != ns_projection_model::wdk)
            {
                if (!needed_modules.empty() && !needed_modules.contains(module_name)) continue;
            }
            if (emitted_modules.contains(module_name))
            {
                continue;
            }
            emitted_modules.insert(module_name);

            std::vector<std::string> members;
            auto mit = groups.find(owner);
            if (mit != groups.end())
            {
                // The SCC-group job owns its member namespaces — in every
                // mode — but ONLY the ones of its own projection model.
                // A merged group can span models (a WDK cycle through the
                // root namespace pulls Windows.Wdk.* into the same SCC as
                // WinRT namespaces): giving the winrt-model job the WDK
                // members emitted Windows::Wdk::BTHDDI a second time under
                // the winrt:: prefix, which cannot compile (wrong model,
                // C2079/C2371/C2039) and duplicates the clean wdk-model
                // twin job that already owns them. Each twin takes exactly
                // its model's members; per-namespace import names are
                // served by the re-export shells.
                bool job_w32 = (model == ns_projection_model::win32 ||
                                model == ns_projection_model::wdk);
                for (auto const& mem : mit->second)
                {
                    auto mem_model = classify_namespace(mem);
                    bool mem_w32 = (mem_model == ns_projection_model::win32 ||
                                    mem_model == ns_projection_model::wdk);
                    if (mem_w32 == job_w32) members.push_back(mem);
                }
            }
            else
            {
                // Namespaces absent from the SCC grouping own exactly
                // themselves.
                members.push_back(ns);
            }
            jobs.push_back(module_job{});
            auto& job = jobs.back();
            job.module_name = std::move(module_name);
            job.model = model;
            job.members = std::move(members);
            job.ordered = order_group_members(job.members, ns_deps);
            if (settings.verbose && (job.module_name == "Windows.Win32" || job.module_name == "winrt.Windows.Win32"))
            {
                std::printf("  [trace] job created: module=%s model=%d members=%zu (from ns %s, owner %s)\n",
                    job.module_name.c_str(), (int)job.model, job.members.size(), ns.c_str(), owner.c_str());
            }
        }

        // Split large modules into sub-modules for faster compilation.
        // A single namespace like Microsoft.UI.Xaml can contain 50K+ types;
        // splitting it into ~5K-type chunks produces modules that compile
        // in seconds instead of minutes.  Each sub-module is an independent
        // `export module` that imports the previous chunk, so consumers
        // who import the root module get everything via re-export.
        {
            // Methods per split chunk. 50K methods ≈ a 320K-line TU that
            // needs gigabytes of front-end memory; CPPWINC_MAX_MEMBERS can
            // lower it so each cl.exe fits the machine's RAM.
            std::size_t MAX_MEMBERS = 50000;
            if (char const* mm = std::getenv("CPPWINC_MAX_MEMBERS"))
            {
                unsigned long n = std::strtoul(mm, nullptr, 10);
                if (n >= 1000) MAX_MEMBERS = static_cast<std::size_t>(n);
            }
            constexpr std::size_t SKIP_SPLIT_TYPE_COUNT = 500;
            std::vector<module_job> split;
            for (auto const& job : jobs)
            {
                // Only WinRT modules are split: Win32/WDK group modules stay
                // whole (their namespaces interdepend through one SCC group).
                if (job.model != ns_projection_model::winrt)
                {
                    split.push_back(job);
                    continue;
                }
                // Quick check: if total type count is small, splitting is never needed
                std::size_t total_types = 0;
                for (auto const& ns : job.members)
                {
                    auto it = metadata.namespaces().find(ns);
                    if (it == metadata.namespaces().end()) continue;
                    auto const& info = it->second;
                    total_types += info.classes.size() + info.interfaces.size()
                                 + info.enums.size() + info.structs.size()
                                 + info.delegates.size();
                }
                if (total_types < SKIP_SPLIT_TYPE_COUNT)
                {
                    split.push_back(job);
                    continue;
                }
                // count projected methods (direct + inherited) across all namespaces
                std::size_t total_methods = 0;
                for (auto const& ns : job.members)
                {
                    auto it = metadata.namespaces().find(ns);
                    if (it == metadata.namespaces().end()) continue;
                    auto const& info = it->second;
                    auto count_type = [&](winmd::type_info const* t)
                    {
                        if (!t || !in_projection(*t)) return;
                        // direct methods
                        for (auto const* iface : t->interfaces)
                        {
                            if (!iface || !iface->is_interface()) continue;
                            auto methods = flattened_methods(*iface, metadata);
                            total_methods += methods.size();
                        }
                        // inherited methods (walk base chain)
                        winmd::type_info const* base = t->base;
                        std::set<winmd::type_info const*> visited_bases;
                        while (base && visited_bases.insert(base).second)
                        {
                            for (auto const* bi : base->interfaces)
                            {
                                if (!bi || !bi->is_interface()) continue;
                                auto methods = flattened_methods(*bi, metadata);
                                total_methods += methods.size();
                            }
                            base = base->base;
                        }
                    };
                    for (auto const* t : info.classes) count_type(t);
                    for (auto const* t : info.interfaces) count_type(t);
                }
                if (total_methods <= MAX_MEMBERS)
                {
                    if (settings.verbose) std::printf("  module %s: %zu methods (no split)\n", job.module_name.c_str(), total_methods);
                    split.push_back(job);
                    continue;
                }
                std::size_t n_chunks = (total_methods + MAX_MEMBERS - 1) / MAX_MEMBERS;
                if (settings.verbose) std::printf("  module %s: %zu methods -> splitting into %zu chunks\n", job.module_name.c_str(), total_methods, n_chunks);
                // Sort namespaces by type count (descending) for balanced distribution
                std::vector<std::pair<std::string, std::size_t>> ns_sizes;
                for (auto const& ns : job.ordered)
                {
                    auto it = metadata.namespaces().find(ns);
                    std::size_t sz = 0;
                    if (it != metadata.namespaces().end())
                    {
                        auto const& info = it->second;
                        sz = info.classes.size() + info.interfaces.size()
                           + info.enums.size() + info.structs.size()
                           + info.delegates.size();
                    }
                    ns_sizes.push_back({ns, sz});
                }
                std::sort(ns_sizes.begin(), ns_sizes.end(),
                          [](auto const& a, auto const& b) { return a.second > b.second; });
                // Distribute namespaces round-robin across chunks (smallest first)
                // Root module becomes a thin re-export shell; all types live
                // in sub-modules so consumers still get everything via import.
                std::vector<std::vector<std::string>> chunks(n_chunks);
                std::vector<std::size_t> chunk_sizes(n_chunks, 0);
                for (auto const& [ns, sz] : ns_sizes)
                {
                    // put in the smallest chunk (keeps root small)
                    auto min_it = std::min_element(chunk_sizes.begin(), chunk_sizes.end());
                    std::size_t ci = min_it - chunk_sizes.begin();
                    chunks[ci].push_back(ns);
                    chunk_sizes[ci] += sz;
                }
                // chunk 0 = root (re-export only), chunks 1..N = type-bearing sub-modules
                // swap: make chunk 0 the smallest (re-export shell)
                std::size_t smallest = 0;
                for (std::size_t ci = 1; ci < n_chunks; ++ci)
                    if (chunk_sizes[ci] < chunk_sizes[smallest]) smallest = ci;
                if (smallest != 0) std::swap(chunks[0], chunks[smallest]);
                // Root must be EMPTY: move any content from chunk 0 to chunk 1
                // (skip_types=true for root would lose these types otherwise)
                if (!chunks[0].empty() && n_chunks > 1)
                {
                    for (auto const& ns : chunks[0])
                        chunks[1].push_back(ns);
                    chunks[0].clear();
                }
                for (std::size_t ci = 0; ci < n_chunks; ++ci)
                {
                    module_job sub;
                    if (ci == 0)
                    {
                        sub.module_name = job.module_name;
                        // Always emit root even if chunk is empty: P modules import it
                    }
                    else
                    {
                        sub.module_name = job.module_name + ".P" + std::to_string(ci);
                    }
                    if (chunks[ci].empty() && ci > 0) continue;
                    sub.members = chunks[ci];
                    sub.ordered = sub.members;
                    if (ci > 0)
                    {
                        std::string root_key = job.module_name.starts_with("winrt.")
                            ? job.module_name.substr(6) : job.module_name;
                        for (auto const& ns : chunks[ci])
                            p_chunk_members[root_key][ns] = root_key + ".P" + std::to_string(ci);
                    }
                    split.push_back(std::move(sub));
                }

            }
            jobs = std::move(split);
        }

        // Register split sub-modules in emitted_modules so the root
        // thin-re-export shell can find them (has_subs check)
        for (auto const& job : jobs)
        {
            emitted_modules.insert(job.module_name);
        }

        // Decide full-vs-selective BEFORE the winrt_base bookkeeping insert
        // (needed_modules stays empty in full-mode runs until then; the shim
        // loop below must see the true full-mode state).
        bool full_mode = needed_modules.empty();

        // Always include winrt_base for WinRT modules
        for (auto const& job : jobs)
        {
            if (job.model == ns_projection_model::winrt)
            {
                needed_modules.insert("winrt_base");
                break;
            }
        }

        // Emit thin re-export modules for sub-namespaces absorbed by
        // SCC merge into a parent module.  E.g. if Microsoft.UI.Xaml.Controls
        // was merged into winrt.Microsoft.UI.Xaml, views that
        // `import winrt.Microsoft.UI.Xaml.Controls` still need that module to exist.
        for (auto const& [ns, owner] : ns_owner)
        {
            if (ns == owner) continue;
            
            // Use correct module name prefix based on namespace type
            auto model = classify_namespace(ns);
            std::string reexport_name;
            std::string owner_module;
            
            if (model == ns_projection_model::win32 || model == ns_projection_model::wdk)
            {
                // Win32/WDK: module name IS the namespace - but mangle it
                // like every other module name (Windows.Wdk.1394 has a
                // digit-led segment that is not a legal C++ module name
                // identifier; MSVC rejects `export module Windows.Wdk.1394;`
                // with C2143). sanitize_module_name keeps decl+importers
                // in agreement.
                reexport_name = sanitize_module_name(ns);
                owner_module = owner;
            }
            else
            {
                // WinRT: module name has "winrt." prefix
                reexport_name = "winrt." + ns;
                owner_module = "winrt." + owner;
            }
            
            // Full-mode runs keep every module: emit re-export shells for all
            // absorbed namespaces. Selective mode only emits the ones the
            // project actually imports. Full-vs-selective must be decided by
            // the state BEFORE the winrt_base bookkeeping insert below (which
            // made needed_modules permanently non-empty and silently skipped
            // every shell — e.g. winrt.Windows.Storage — leaving consumers
            // unable to import absorbed namespaces).
            if (!full_mode && !needed_modules.contains(reexport_name)) continue;
            if (emitted_modules.contains(reexport_name)) continue;
            emitted_modules.insert(reexport_name);
            
            // Find the actual emitted module that owns this namespace
            // (could be a split sub-module name like winrt.Microsoft.UI.Xaml.P1)
            // Check if owner module exists in jobs
            bool owner_exists = false;
            for (auto const& job : jobs)
            {
                if (job.module_name == owner_module) { owner_exists = true; break; }
            }
            if (!owner_exists) continue;
            
            // Emit re-export stub
            writer rw;
            rw.write("// Auto-generated re-export: % is merged into %\n", ns, owner_module);
            rw.write("export module %;\n", reexport_name);
            rw.write("export import %;\n", owner_module);
            write_output_file(output_folder / (reexport_name + ".ixx"), rw.str());
        }        // bodies of component-interface methods that reference a
        // component class: emitted into the impl module after the class
        // definition (the projection module only forward-declares the
        // class, so these cannot compile there). Mutex-guarded because
        // module emission runs in parallel.
        std::string component_bodies;
        std::mutex component_bodies_mutex;

        auto emit_module = [&](module_job const& job)
        {
            // Private mapper per module: module emission runs in parallel and
            // the projection model (0 = WinRT, 1 = Win32/WDK) plus the owner
            // namespace are per-module context. Sharing one mutable mapper
            // across threads lets a WinRT job stomp a WDK job's model mid-
            // emission, leaking winrt:: names into WDK modules (those modules
            // are plain C++ without the winrt runtime) and vice versa. Shadow
            // the outer mapper with a copy.
            type_mapper mapper{ metadata };

            // Extract owner namespace from module name
            std::string owner;
            if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
            {
                // Win32/WDK: module name IS the namespace (e.g. "Windows.Win32.FileSystem")
                owner = job.module_name;
            }
            else
            {
                // WinRT: strip "winrt." prefix
                owner = job.module_name.substr(6);
            }
            writer w;
            mapper.set_owner_ns(owner); // set module context for short-name resolution
            if (settings.verbose && (job.module_name == "Windows.Win32" || job.module_name == "winrt.Windows.Win32"))
            {
                std::printf("  [trace] emit_module: module=%s model=%d members=%zu\n",
                    job.module_name.c_str(), (int)job.model, job.members.size());
            }
            // Use appropriate module preamble based on namespace type
            if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
            {
                write_win32_wdk_module_preamble(w, job.module_name, job.model == ns_projection_model::wdk);
            }
            else
            {
                write_module_preamble(w, job.module_name);
            }

            std::set<std::string, std::less<>> dep_owners;
            for (auto const& m : job.members)
            {
                auto it = ns_deps.find(m);
                if (it == ns_deps.end())
                {
                    if (std::getenv("CPPWINC_DEPTRACE")) std::fprintf(stderr, "[dep] %s: no ns_deps for member %s\n", job.module_name.c_str(), m.c_str());
                    continue;
                }
                for (auto const& d : it->second)
                {
                    if (std::getenv("CPPWINC_DEPTRACE")) std::fprintf(stderr, "[dep] %s: member %s -> dep %s (in_nodes=%d owner=%s)\n", job.module_name.c_str(), m.c_str(), d.c_str(), (int)in_nodes.contains(d), ns_owner[d].c_str());
                    if (!in_nodes.contains(d)) continue;
                    // Imports always name the dep's SCC-group module via
                    // ns_owner. Naming the dep namespace directly (the old
                    // selective-mode bypass) recreated the Win32 namespace
                    // import cycles that SCC merging exists to break.
                    dep_owners.insert(ns_owner[d]);
                }
            }
            if (std::getenv("CPPWINC_DEPTRACE") && !dep_owners.empty())
            {
                for (auto const& d : dep_owners) std::fprintf(stderr, "[dep] %s: final dep_owner %s\n", job.module_name.c_str(), d.c_str());
            }
            // For split sub-modules (P1, P2, ...), skip importing the root
            // owner to avoid circular deps (root imports P*, P* imports root).
            // The .P marker must be a trailing chunk suffix (.P1, .P12, ...):
            // a plain substring find(".P") also matches real namespaces like
            // SocialInfo.Provider / Xaml.Printing, which then wrongly skip the
            // import of their parent namespace module (the root owner).
            auto is_p_chunk = [](std::string const& mn)
            {
                auto pos = mn.rfind(".P");
                if (pos == std::string::npos) return false;
                if (pos + 2 >= mn.size()) return false;
                for (std::size_t i = pos + 2; i < mn.size(); ++i)
                    if (!std::isdigit(static_cast<unsigned char>(mn[i]))) return false;
                return true;
            };
            bool is_split_sub = is_p_chunk(job.module_name);
            // The root owner is the base module name (e.g. "Microsoft.UI.Xaml")
            std::string root_owner;
            if (is_split_sub) {
                auto pos = owner.rfind(".P");
                if (pos != std::string::npos) root_owner = owner.substr(0, pos);
            }
            // A COM interface IID constant (`inline constexpr GUID IID_IFoo =
            // {...}`) hardcodes the GUIDDEF type, so no metadata signature            // reference registers the dep. Seed GUIDDEF here whenever this module
            // emits any such IID, else the const fails C3083/C2039 ("GUID is not
            // a member of Windows::Win32") in every COM-carrying module.
            if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
            {
                for (auto const& m : job.members)
                {
                    auto mit = metadata.namespaces().find(m);
                    if (mit == metadata.namespaces().end()) continue;
                    auto const& minfo = mit->second;
                    for (auto const* t : minfo.interfaces)
                    {
                        if (t && in_projection(*t) && t->com_interface && t->has_com_iid)
                        {
                            dep_owners.insert("Windows.Win32.GUIDDEF");
                            break;
                        }
                    }
                }
            }
            for (auto const& d : dep_owners)
            {
                // Only import modules that were actually generated (in needed_modules)
                // Use correct module name prefix based on projection model
                std::string dep_module_name;
                if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
                {
                    // Win32/WDK: module name IS the namespace
                    dep_module_name = sanitize_module_name(d);
                }
                else
                {
                    // WinRT: module name has "winrt." prefix
                    dep_module_name = "winrt." + d;
                }
                
                // Import any dependency module that was actually emitted.
                // Full-mode runs emit every module, so this resolves every
                // cross-module type reference. Selective (-winui / module
                // filters) runs emit only the pruned set, so the modules that
                // exist are exactly the ones that may be imported. (Gating on
                // needed_modules.empty() is wrong: winrt_base is always
                // inserted below, so the set is never empty and full-mode
                // runs silently skipped every cross-module import.)
                bool dep_ok = emitted_modules.contains(dep_module_name)
                    || planned_modules.contains(dep_module_name);
                // Compare the SANITIZED dep name against this module's name:
                // `d != owner` compared the raw namespace (BITS2.5) with the
                // mangled module name (BITS2_5), so digit-led namespaces
                // slipped a self-import past the guard (`export import
                // Windows.Win32.BITS2_5;` inside the module itself -> C2230
                // "could not find module" in BITS2_5/BITS5_0/D2D1_1/
                // D3D10_1.SHADER/D3D11_2).
                if (dep_module_name == job.module_name) continue;
                if (d != owner && dep_ok)
                {
                    // Skip root owner for P* sub-modules to avoid cycles
                    if (is_split_sub && !root_owner.empty() && d == root_owner) continue;
                    w.write("export import %;\n", dep_module_name);
                }
            }
            // For P* sub-modules, import only the specific sibling P*
            // sub-modules whose namespaces are in our dependency set.
            // Importing ALL siblings creates circular deps.
            if (is_split_sub && !root_owner.empty())
            {
                // NO sibling imports between P-chunks: namespace deps inside
                // one SCC group are cyclic, so any sibling import would form
                // a module-import cycle (P1 -> P2 -> P1), which C++ modules
                // forbid. Each chunk compiles standalone against the group's
                // module-level dependencies, exactly as the pre-split whole
                // namespace did. Cross-chunk signature references are
                // resolved because chunk boundaries follow namespace order.
            }
            // Root module: thin re-export shell that imports all sub-modules
            bool skip_types = false;
            bool has_subs = false;
            {
                bool is_root = !is_p_chunk(job.module_name);
                for (int pi = 1; ; ++pi)
                {
                    std::string sub = owner + ".P" + std::to_string(pi);
                    if (emitted_modules.contains("winrt." + sub))
                    {
                        w.write("export import winrt.%;\n", sub);
                        has_subs = true;
                    }
                    else break;
                }
                // Root re-exports only P* chunks and dep_owners modules.
                // Sibling sub-modules (e.g. winrt.Windows.Foundation) are
                // imported by consumers directly, keeping the build graph small.
                // If root has sub-modules, it's a thin re-export shell: all
                // types live in its .P* sub-modules, so skip type emission.
                skip_types = is_root && has_subs;
            }
            if (!skip_types)
            {
            w.write("\n");

            // forward declarations for every projected type in the module,
            // so cross-namespace references resolve regardless of order
            auto forward = [&](winmd::namespace_info const& info)
            {
                for (auto const* t : info.enums)
                {
                    if (!in_projection(*t)) continue;
                    std::string underlying = enum_underlying_cpp(t->enum_underlying);
                    if (underlying.empty()) underlying = "std::int32_t";
                    w.write("#undef %\n", t->name);
                    w.write("export enum class % : %;\n", t->name, underlying);
                }
                for (auto const* t : info.structs)
                {
                    if (in_projection(*t)) w.write("export struct %;\n", t->name);
                }
                for (auto const* t : info.delegates)
                {
                    if (!in_projection(*t)) continue;
                    if (t->generic_params.empty())
                    {
                        w.write("export struct %;\n", t->name);
                    }
                    else
                    {
                        w.write("export %struct %;\n", tpl_prefix(*t, metadata), t->name);
                    }
                }
                for (auto const* t : info.interfaces)
                {
                    if (!in_projection(*t)) continue;
                    if (t->generic_params.empty())
                    {
                        w.write("export struct %;\n", t->name);
                    }
                    else
                    {
                        w.write("export %struct %;\n", tpl_prefix(*t, metadata), t->name);
                    }
                }
                for (auto const* t : info.classes)
                {
                    // component classes are forward-declared too: their
                    // own interfaces (e.g. a statics factory returning
                    // the class) reference them by name in signatures,
                    // which must resolve in the projection module
                    if (!in_projection(*t)) continue;
                    // Win32/WDK function-classes (static methods only) are
                    // emitted as inline functions, not types — skip them
                    bool is_function_class = !t->methods.empty();
                    if (is_function_class &&
                        (classify_namespace(t->ns) == ns_projection_model::win32 ||
                         classify_namespace(t->ns) == ns_projection_model::wdk))
                        continue;
                    if (t->generic_params.empty())
                    {
                        w.write("export struct %;\n", t->name);
                    }
                    else
                    {
                        w.write("export %struct %;\n", tpl_prefix(*t, metadata), t->name);
                    }
                }
            };
            // Forward-declare types in THIS module's namespaces.
            // Win32/WDK: interfaces reference each other in cycles (IFileDialog
            // -> IFileDialogEvents) and STRUCTS are emitted in a later pass
            // than the interfaces that use them, so declaration order alone
            // can never satisfy first use (C2039 otherwise). Only interfaces
            // and structs are forward-declared here — delegates and handle
            // classes are `export using` aliases in Win32 (a `struct` fwd
            // would conflict with the alias — why this path was disabled).
            if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
            {
                for (auto const& m : job.ordered)
                {
                    auto nit = metadata.namespaces().find(m);
                    if (nit == metadata.namespaces().end()) continue;
                    w.write("namespace %\n{\n", win32_ns_cpp(m));
                    // Enums too: aliases/fnptrs referencing an enum defined
                    // later in the module (ICU UBiDiClassCallback ->
                    // UCharDirection) need the fwd decl; winmd always has
                    // the fixed underlying type.
                    for (auto const* t : nit->second.enums)
                    {
                        if (!in_projection(*t)) continue;
                        std::string underlying = enum_underlying_cpp(t->enum_underlying);
                        if (underlying.empty()) underlying = "std::int32_t";
                        w.write("#undef %\n", t->name);
                        w.write("export enum class % : %;\n", t->name, underlying);
                    }
                    for (auto const* t : nit->second.interfaces)
                    {
                        if (!in_projection(*t)) continue;
                        if (t->generic_params.empty())
                            w.write("export struct %;\n", t->name);
                        else
                            w.write("export %struct %;\n", tpl_prefix(*t, metadata), t->name);
                    }
                    for (auto const* t : nit->second.structs)
                    {
                        if (!in_projection(*t)) continue;
                        if (t->generic_params.empty())
                            w.write("export struct %;\n", t->name);
                        else
                            w.write("export %struct %;\n", tpl_prefix(*t, metadata), t->name);
                    }
                    w.write("}\n\n");
                }
            }
            else
            {
            for (auto const& m : job.ordered)
            {
                // Use correct namespace prefix based on projection model
                std::string ns_prefix;
                if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
                    ns_prefix = win32_ns_cpp(m);
                else
                    ns_prefix = ns_cpp(m);
                w.write("namespace %\n{\n", ns_prefix);
                forward(metadata.namespaces().find(m)->second);
                w.write("}\n\n");
            }
            } // end forward-declare block
            // Also forward-declare types in OTHER namespaces so P-chunks
            // referencing types from sibling chunks compile.
            // Only emit when this module has been split (has_subs).
            if (has_subs)
            {
            for (auto const& [ns_name, ns_info] : metadata.namespaces())
            {
                bool in_job = false;
                for (auto const& jn : job.ordered)
                    if (jn == ns_name) { in_job = true; break; }
                if (in_job) continue;
                bool has_proj = false;
                for (auto const* t : ns_info.classes)
                    if (in_projection(*t)) { has_proj = true; break; }
                if (!has_proj)
                    for (auto const* t : ns_info.interfaces)
                        if (in_projection(*t)) { has_proj = true; break; }
                if (!has_proj)
                    for (auto const* t : ns_info.enums)
                        if (in_projection(*t)) { has_proj = true; break; }
                if (!has_proj) continue;
                // Use correct namespace prefix based on projection model
                std::string ns_prefix;
                if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
                    ns_prefix = win32_ns_cpp(ns_name);
                else
                    ns_prefix = ns_cpp(ns_name);
                w.write("namespace %\n{\n", ns_prefix);
                forward(ns_info);
                w.write("}\n\n");
            }
            } // end if (has_subs)
            w.write("\n");

            std::string winrt_specs;
            std::string bodies;
            std::string async_helpers;
            std::string co_await_text;
            bool module_has_async = false;
            bool async_helpers_emitted = false;
            // Module-level function-name dedup: a merged module can span
            // several namespaces (SCC grouping); C functions cannot overload,
            // so one name set per module keeps the first declaration only.
            std::set<std::string> flat_functions_emitted;
            for (auto const& m : job.ordered)
            {
                auto const& info = metadata.namespaces().find(m)->second;
                // Use correct namespace prefix based on projection model
                std::string ns_prefix;
                if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
                    ns_prefix = win32_ns_cpp(m);
                else
                    ns_prefix = ns_cpp(m);
                w.write("namespace %\n{\n", ns_prefix);

                // Route types to appropriate projection writers based on namespace type
                mapper.set_proj_model(1); // Win32/WDK context for type_mapper
                if (job.model == ns_projection_model::win32)
                {
                    // Win32 projection. Delegates come FIRST: a struct field
                    // can be typed as a callback pointer (SERVICE_ASYNC_INFO
                    // holds LPSERVICE_CALLBACK_PROC), and C++ `using` alias
                    // declarations must appear before the use inside the
                    // same namespace block.
                    for (auto const* t : info.delegates)
                    {
                        if (in_projection(*t)) write_win32_delegate(w, *t, mapper, metadata, flat_functions_emitted);
                    }
                    for (auto const* t : info.enums)
                    {
                        if (in_projection(*t)) write_win32_enum(w, *t, mapper);
                    }
                    // Structs are deferred to the module-wide pass below:
                    // a merged module spans namespaces whose structs have
                    // cross-namespace VALUE members (CLFS -> NSPAPI::GUID),
                    // and a dep cycle at namespace level makes any namespace
                    // ordering impossible. Only a global type-level topo can
                    // order them.
                    (void)0;
                    for (auto const* t : info.interfaces)
                    {
                        if (in_projection(*t)) write_win32_com_interface(w, *t, mapper, metadata);
                    }
                    // Opaque handle classes (DECLARE_HANDLE results):
                    // field-less/method-less classes whose names appear in
                    // function signatures (PACKAGEDEPENDENCY_CONTEXT,
                    // HSTRING_BUFFER, CO_MTA_USAGE_COOKIE, ...). Without
                    // emission every signature referencing them fails with
                    // C2039 'not a member'. Type signatures in Win32 context
                    // already map member-less classes to void*; the alias
                    // keeps the ABI void* while giving the named type a
                    // definition inside its own namespace.
                    for (auto const* t : info.classes)
                    {
                        if (!in_projection(*t)) continue;
                        if (!t->fields.empty() || !t->methods.empty() ||
                            !t->interfaces.empty() || !t->properties.empty())
                            continue;
                        if (!t->coclass_clsid.empty())
                            w.write("// COM coclass CLSID %\n", t->coclass_clsid);
                        if (is_valid_alias_name(t->name))
                        {
                            // Scalar typedef aliases (DWORD, BOOL, RPC_STATUS,
                            // UINT ...) carry AliasTarget = ECMA element type
                            // code; without it the field-less row is
                            // indistinguishable from an opaque handle, which
                            // flattens to void* by design (README).
                            std::uint8_t at_code = 0; std::uint32_t at_ptr = 0;
                            if (!t->alias_target.empty())
                            {
                                std::size_t bar = t->alias_target.find('|');
                                std::string at_c = t->alias_target.substr(0, bar);
                                for (char ac : at_c)
                                {
                                    if (ac < '0' || ac > '9') { at_code = 0; break; }
                                    at_code = static_cast<std::uint8_t>(at_code * 10 + (ac - '0'));
                                }
                                if (bar != std::string::npos)
                                {
                                    for (std::size_t pi = bar + 1;
                                         pi < t->alias_target.size() && at_ptr < 4; ++pi)
                                    {
                                        char pc = t->alias_target[pi];
                                        if (pc < '0' || pc > '9') break;
                                        at_ptr = at_ptr * 10 + static_cast<std::uint32_t>(pc - '0');
                                    }
                                }
                            }
                            std::string at_cpp;
                            if (at_code && !win32_handle_alias(t->name))
                            {
                                // Map through the type mapper (not primitive_cpp)
                                // for the context-correct C spelling: ET::Char is
                                // wchar_t in Win32/WDK modules, not char16_t.
                                winmd::type at_t{};
                                at_t.kind = at_code;
                                at_cpp = mapper.map(at_t);
                                for (std::uint32_t s = 0; s < at_ptr && s < 4; ++s) at_cpp += '*';
                            }
                            if (!at_cpp.empty() && at_cpp != "void")
                                w.write("export using % = %;\n\n", t->name, at_cpp);
                            else
                                w.write("export using % = void*;\n\n", t->name);
                        }
                    }
                    // Flat functions are deferred to a second pass below:
                    // a merged Win32 module spans several namespaces and a
                    // function in one (CHAKRART) may take enums/structs from
                    // another (JSRT9) that job.ordered places later.
                }
                else if (job.model == ns_projection_model::wdk)
                {
                    mapper.set_proj_model(1); // Win32/WDK context for type_mapper
                    // WDK projection (delegates before structs, see the
                    // Win32 branch above for the ordering rationale).
                    for (auto const* t : info.delegates)
                    {
                        if (in_projection(*t)) write_win32_delegate(w, *t, mapper, metadata, flat_functions_emitted);
                    }
                    for (auto const* t : info.enums)
                    {
                        if (in_projection(*t)) write_wdk_enum(w, *t, mapper);
                    }
                    // Structs deferred (see the Win32 branch above).
                    (void)0;
                    for (auto const* t : info.interfaces)
                    {
                        if (in_projection(*t)) write_win32_com_interface(w, *t, mapper, metadata);
                    }
                    // Opaque handle classes (see the Win32 branch above).
                    for (auto const* t : info.classes)
                    {
                        if (!in_projection(*t)) continue;
                        if (!t->fields.empty() || !t->methods.empty() ||
                            !t->interfaces.empty() || !t->properties.empty())
                            continue;
                        if (!t->coclass_clsid.empty())
                            w.write("// COM coclass CLSID %\n", t->coclass_clsid);
                        if (is_valid_alias_name(t->name))
                        {
                            // Scalar typedef aliases (DWORD, BOOL, RPC_STATUS,
                            // UINT ...) carry AliasTarget = ECMA element type
                            // code; without it the field-less row is
                            // indistinguishable from an opaque handle, which
                            // flattens to void* by design (README).
                            std::uint8_t at_code = 0; std::uint32_t at_ptr = 0;
                            if (!t->alias_target.empty())
                            {
                                std::size_t bar = t->alias_target.find('|');
                                std::string at_c = t->alias_target.substr(0, bar);
                                for (char ac : at_c)
                                {
                                    if (ac < '0' || ac > '9') { at_code = 0; break; }
                                    at_code = static_cast<std::uint8_t>(at_code * 10 + (ac - '0'));
                                }
                                if (bar != std::string::npos)
                                {
                                    for (std::size_t pi = bar + 1;
                                         pi < t->alias_target.size() && at_ptr < 4; ++pi)
                                    {
                                        char pc = t->alias_target[pi];
                                        if (pc < '0' || pc > '9') break;
                                        at_ptr = at_ptr * 10 + static_cast<std::uint32_t>(pc - '0');
                                    }
                                }
                            }
                            std::string at_cpp;
                            if (at_code && !win32_handle_alias(t->name))
                            {
                                // Map through the type mapper (not primitive_cpp)
                                // for the context-correct C spelling: ET::Char is
                                // wchar_t in Win32/WDK modules, not char16_t.
                                winmd::type at_t{};
                                at_t.kind = at_code;
                                at_cpp = mapper.map(at_t);
                                for (std::uint32_t s = 0; s < at_ptr && s < 4; ++s) at_cpp += '*';
                            }
                            if (!at_cpp.empty() && at_cpp != "void")
                                w.write("export using % = %;\n\n", t->name, at_cpp);
                            else
                                w.write("export using % = void*;\n\n", t->name);
                        }
                    }
                    // Flat functions deferred (see the Win32 branch above).
                }
                else
                {
                    mapper.set_proj_model(0); // WinRT context for type_mapper
                    // WinRT projection (existing logic)
                    for (auto const* t : info.enums)
                    {
                        if (in_projection(*t)) write_enum(w, *t, mapper, winrt_specs);
                    }
                    {
                    // structs may reference other structs of the same
                    // namespace as value members; emit in dependency
                    // order (Kahn's algorithm)
                    std::vector<winmd::type_info const*> structs;
                    for (auto const* t : info.structs)
                    {
                        if (in_projection(*t)) structs.push_back(t);
                    }
                    std::map<winmd::type_info const*, std::set<winmd::type_info const*>> field_deps;
                    std::set<winmd::type_info const*> remaining(structs.begin(), structs.end());
                    for (auto const* t : structs)
                    {
                        for (auto const& f : t->fields)
                        {
                            std::vector<winmd::type const*> stack{ &f.value };
                            while (!stack.empty())
                            {
                                auto const* ty = stack.back();
                                stack.pop_back();
                                if (ty->kind == winmd::et_class || ty->kind == winmd::et_valuetype || ty->kind == winmd::et_genericinst)
                                {
                                    if (auto const* dep = metadata.find(*ty))
                                    {
                                        if (dep->is_struct() && remaining.contains(dep))
                                        {
                                            field_deps[t].insert(dep);
                                        }
                                    }
                                }
                                for (auto const& a : ty->args) stack.push_back(&a);
                                for (auto const& c : ty->children) stack.push_back(&c);
                            }
                        }
                    }
                    std::set<winmd::type_info const*> done;
                    while (done.size() < structs.size())
                    {
                        bool progress = false;
                        for (auto const* t : structs)
                        {
                            if (done.contains(t)) continue;
                            bool ready = true;
                            for (auto const* d : field_deps[t])
                            {
                                if (!done.contains(d)) { ready = false; break; }
                            }
                            if (ready)
                            {
                                write_struct(w, *t, mapper, metadata, winrt_specs);
                                done.insert(t);
                                progress = true;
                            }
                        }
                        if (!progress) break;
                    }
                    for (auto const* t : structs)
                    {
                        if (!done.contains(t)) write_struct(w, *t, mapper, metadata, winrt_specs);
                    }
                }
                for (auto const* t : info.delegates)
                {
                    if (in_projection(*t)) write_delegate(w, *t, mapper, metadata, winrt_specs, bodies);
                }
                for (auto const* t : info.interfaces)
                {
                    if (in_projection(*t)) write_interface(w, *t, mapper, metadata, winrt_specs, bodies, component_bodies, component_bodies_mutex, module_has_async, co_await_text);
                }
                // IReserveableVector<T> is a proposal interface that no OS
                // winmd carries, so synthesize it here whenever the
                // namespace projects IVector. Sibling interface (like
                // IIterable beside IVector): base stays null so the slot
                // math lands Reserve at 6 + 0 + 0 = slot 6 - winrt_base's
                // reserveable_iface vtable (IInspectable 0-5 + Reserve) -
                // and interface_method_count=1 sizes
                // interface_impl_base<D,I>::m_methods for authoring
                // (slot 6+i is exactly m_methods[i]).
                {
                    bool already = false;
                    winmd::type_info const* ivector = nullptr;
                    for (auto const* t : info.interfaces)
                    {
                        if (!t) continue;
                        if (t->name == "IReserveableVector") already = true;
                        if (t->name == "IVector" && !t->generic_params.empty()) ivector = t;
                    }
                    if (!already && ivector && in_projection(*ivector))
                    {
                        winmd::type_info synth;
                        synth.ns = m;
                        synth.name = "IReserveableVector"; // type names strip the `1 arity at load
                        synth.kind = ivector->kind; // interface (avoid the SDK `interface` macro here)
                        synth.flags = ivector->flags;
                        synth.db = ivector->db;
                        synth.row = ivector->row;
                        synth.generic_params = ivector->generic_params; // T0
                        // 3f7a19c2-6b1e-4d95-8a24-d15e9c47b308 in winmd blob
                        // order (guid_literal prints guid[3..0] as the u32).
                        static constexpr std::uint8_t reserve_guid[16] = {
                            0xc2, 0x19, 0x7a, 0x3f, 0x1e, 0x6b, 0x95, 0x4d,
                            0x8a, 0x24, 0xd1, 0x5e, 0x9c, 0x47, 0xb3, 0x08 };
                        for (std::size_t i = 0; i < 16; ++i) synth.guid[i] = reserve_guid[i];
                        synth.has_guid = true;
                        winmd::method_info reserve;
                        reserve.name = "Reserve";
                        reserve.ret.kind = winmd::et_void;
                        winmd::param_info cap;
                        cap.name = "capacity";
                        cap.value.kind = winmd::et_u4;
                        cap.flags = winmd::param_in;
                        cap.sequence = 1;
                        reserve.params.push_back(std::move(cap));
                        reserve.flags = ivector->methods.empty() ? 0 : ivector->methods[0].flags;
                        reserve.is_virtual = true;
                        reserve.is_abstract = true;
                        synth.methods.push_back(std::move(reserve));
                        write_interface(w, synth, mapper, metadata, winrt_specs, bodies,
                            component_bodies, component_bodies_mutex, module_has_async, co_await_text);
                    }
                }
                if (module_has_async && !async_helpers_emitted)
                {
                    // The awaiters follow every async interface (their
                    // bodies use the complete projected types) and precede
                    // the co_await operators (which return them). They are
                    // emitted in winrt::impl, hence the namespace dance.
                    // A module groups several namespaces, so emit once.
                    async_helpers_emitted = true;
                    w.write("}\n\nexport namespace winrt::impl\n{\n");
                    write_async_awaiters(w, async_helpers);
                    w.write("%", async_helpers);
                    w.write("}\n\nnamespace %\n{\n", ns_cpp(m));
                    if (!co_await_text.empty())
                    {
                        w.write("%", co_await_text);
                    }
                }
                for (auto const* t : info.classes)
                {
                    if (settings.component && is_component_type(*t))
                    {
                        continue;
                    }
                    if (in_projection(*t)) write_class(w, *t, mapper, metadata, winrt_specs, bodies);
                }
                } // end else (WinRT projection)
                w.write("}\n\n");
            } // end for each namespace in job

            // Module-wide struct pass (Win32/WDK): ALL structs of the merged
            // module in value-field dependency order, each wrapped in its own
            // namespace block. Cross-namespace value members (CLFS ->
            // NSPAPI::GUID, WinSock -> MINWINBASE::...) require the defining
            // namespace to be complete first; namespace-level ordering cannot
            // achieve that when the namespace dep graph is cyclic — the very
            // reason these namespaces share one module.
            if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
            {
                std::vector<winmd::type_info const*> all_structs;
                for (auto const& m : job.ordered)
                {
                    auto const& info = metadata.namespaces().find(m)->second;
                    for (auto const* t : info.structs)
                        if (in_projection(*t)) all_structs.push_back(t);
                }
                write_structs_topo(w, all_structs, metadata,
                    [&mapper, &metadata, &job](writer& w, winmd::type_info const& t)
                    {
                        w.write("namespace %\n{\n", win32_ns_cpp(t.ns));
                        if (job.model == ns_projection_model::win32)
                            write_win32_struct(w, t, mapper, metadata);
                        else
                            write_wdk_struct(w, t, mapper, metadata);
                        w.write("}\n\n");
                    });
            }

            // Pass 2 for flat C modules: every function, grouped by
            // namespace, after ALL types of the module are complete. C
            // functions cannot overload, so one name set per module keeps
            // the first declaration only (dedup spans both passes).
            if (job.model == ns_projection_model::win32 || job.model == ns_projection_model::wdk)
            {
                for (auto const& m : job.ordered)
                {
                    auto const& info = metadata.namespaces().find(m)->second;
                    mapper.set_proj_model(1);
                    w.write("namespace %\n{\n", win32_ns_cpp(m));
                    // Accumulated across every function-grouping class of
                    // this namespace: an A/W pair can be split between two
                    // metadata wrapper types, so pairing happens here, once
                    // the whole namespace has been emitted.
                    std::vector<std::pair<std::string, aw_fn_sig>> aw_fns;
                    if (job.model == ns_projection_model::win32)
                    {
                        for (auto const* t : info.classes)
                        {
                            if (!in_projection(*t)) continue;
                            write_win32_function(w, *t, mapper, metadata, flat_functions_emitted, aw_fns);
                        }
                    }
                    else
                    {
                        for (auto const* t : info.classes)
                        {
                            if (!in_projection(*t)) continue;
                            if (t->methods.empty()) continue;
                            write_wdk_function(w, *t, mapper, metadata, flat_functions_emitted, aw_fns);
                        }
                    }
                    // Unsuffixed wrapper for every A/W pair in this namespace;
                    // the win32 writer also defers its inline stubs to here.
                    write_aw_canonical(w, win32_ns_cpp(m), aw_fns, job.model == ns_projection_model::win32);
                    w.write("}\n\n");
                }
            }

            // winrt_specs and component_authoring only apply to WinRT modules
            if (job.model == ns_projection_model::winrt)
            {
                if (!winrt_specs.empty())
                {
                    // global ::winrt specializations for exported templates
                    // (non-export: specializations of exported templates are
                    // auto-exported; explicit `export` is ill-formed, C7760)
                    w.write("namespace winrt\n{\n");
                    w.write("%", winrt_specs);
                    w.write("}\n\n");
                }
                if (!bodies.empty())
                {
                    // out-of-line member definitions, emitted after every
                    // projected type is complete (avoids order dependencies)
                    w.write("%", bodies);
                }
                if (job.module_name == "winrt.Windows.Foundation")
                {
                    // component authoring helpers (single_threaded_vector/map,
                    // attach_cancellable, when_all/when_any): emitted into the
                    // Foundation module where the projected collection and
                    // async types are nameable. Only winrt.Windows.Foundation
                    // projects IAsyncAction/IObservableVector — sibling
                    // Foundation modules (e.g. Microsoft.Windows.Foundation,
                    // which carries just DecimalHelper) would C2653 on the
                    // unqualified Windows::Foundation references and cannot
                    // name the types at all.
                    w.write("%", cppwinc::strings::component_authoring_ixx);
                }
            } // end if (job.model == ns_projection_model::winrt)
                } // !skip_types — root shells of split modules stay thin
            write_output_file(output_folder / (job.module_name + ".ixx"), w.str());
        };

        // Emission runs strictly sequentially. A flaky worker-thread access
        // violation (null std::string capacity deref, c0000005) appeared at
        // emission start under parallel scheduling. Every shared-state
        // candidate was audited (writer/mapper/bodies are per-job;
        // emitted_this_run and component_bodies are mutex-guarded; metadata
        // is read-only after the needed phase) yet the race persisted across
        // runs. Serializing removes the failure class entirely; emission is
        // dominated by file I/O so the parallelism win was marginal anyway.
        for (auto const& job : jobs)
        {
            emit_module(job);
        }

        // Remove stale module files from earlier generations (different input
        // set / namespace grouping) that are not part of this run — they would
        // otherwise be compiled/imported by mistake and slow down the build.
        // Only in selective project mode (views present: -winui,
        // -winui_no_compile, or component runs); full SDK projections keep the
        // documented never-delete in-place regeneration behavior. Runs after
        // all output (incl. the component module) has been emitted, and only
        // touches files whose module name follows this generator's naming
        // patterns, so user-authored modules are never deleted.
        if (!needed_modules.empty())
        {
            std::vector<std::filesystem::path> stale;
            for (auto const& entry : std::filesystem::directory_iterator(output_folder))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".ixx") continue;
                std::string stem = entry.path().stem().string();
                if (emitted_this_run.contains(stem)) continue;
                bool generator_named = stem == "winrt_base" || stem == "main"
                    || stem.starts_with("winrt.")
                    || stem.starts_with("Windows.Win32.") || stem.starts_with("Windows.Wdk.")
                    || stem == "Windows.Win32" || stem == "Windows.Wdk"
                    || (!settings.component_name.empty()
                        && (stem == settings.component_name || stem.starts_with(settings.component_name + ".")));
                if (!generator_named) continue;
                // Double-check the module declaration matches the file name
                // before removing anything.
                std::ifstream ifs(entry.path());
                std::string line;
                bool owned = false;
                int scanned = 0;
                std::string needle = "export module " + stem + ";";
                while (std::getline(ifs, line) && scanned++ < 200)
                {
                    if (line.find(needle) != std::string::npos) { owned = true; break; }
                }
                if (owned) stale.push_back(entry.path());
            }
            for (auto const& path : stale)
            {
                std::error_code ec;
                std::filesystem::remove(path, ec);
                if (settings.verbose && !ec)
                    std::printf("cppwinc: removed stale module %s\n", path.filename().string().c_str());
            }
        }

        // component
        if (settings.component && !component_classes.empty())
        {
            std::string component_folder = settings.component_folder.empty() ? settings.output_folder : settings.component_folder;
            std::filesystem::path folder(component_folder);

            // group by namespace, generate impl + projection
            std::map<std::string, std::vector<winmd::type_info const*>, std::less<>> by_ns;
            for (auto const* c : component_classes)
            {
                by_ns[c->ns].push_back(c);
            }
            // .cpp files not needed for -winui mode (modules are self-contained)
            for (auto const& [ns, classes] : by_ns) { (void)ns; (void)classes; }
            // component module (single merged module)
            // Guard against clobbering a projection module of the same name:
            // emit_module already wrote <component_name>.ixx when an SCC group
            // owns that name, and the component file would overwrite it.
            if (settings.verbose && (emitted_modules.contains(settings.component_name) || std::filesystem::exists(std::filesystem::path(settings.output_folder) / (settings.component_name + ".ixx"))))
            {
                std::printf("  [warn] component module name '%s' collides with a projection module; skipping component module write\n", settings.component_name.c_str());
            }
            else
            {
            writer out;
            write_module_preamble(out, settings.component_name);
            std::set<std::string, std::less<>> comp_owners;
            for (auto const& [cns, classes] : by_ns)
            {
                auto it = ns_owner.find(cns);
                comp_owners.insert(it == ns_owner.end() ? cns : it->second);
            }
            for (auto const& owner : comp_owners)
            {
                if (groups.contains(owner))
                {
                    // Import projection module for interface types (IControlExample, etc.)
                    // Only if different namespace — app's own namespace may lack a projection module
                    if (settings.component_name != owner)
                        out.write("import winrt.%;\n", owner);
                    // Component modules use their component name as the module name
                    // Skip self-import (re-export module importing itself)
                    if (settings.component_name != owner)
                        out.write("export import %;\n", settings.component_name);
                }
            }
            // Collect projection modules needed for base types of component classes
            std::set<std::string, std::less<>> needed_proj_modules;
            for (auto const* c : component_classes)
            {
                if (c->base && !is_component_type(*c->base))
                {
                    needed_proj_modules.insert(c->base->ns);
                }
            }
            for (auto const& mod_ns : needed_proj_modules)
            {
                std::string proj_mod = "winrt." + mod_ns;
                if (settings.component_name != mod_ns)
                    out.write("import %;\n", proj_mod);
            }
            out.write("\n");
            for (auto const& [ns, classes] : by_ns)
            {
                out.write("namespace %\n{\n", ns_cpp(ns));
                // forward-declare every component class before the impl
                // bases, so the _base templates can name them
                for (auto const* c : classes)
                {
                    // Determine projected base type for inheritance
                    std::string base_cpp;
                    bool has_projected_base = false;
                    if (c->base && !is_component_type(*c->base))
                    {
                        base_cpp = type_cpp_name(*c->base);
                        has_projected_base = true;
                    }

                    // Emit struct — inherit from projected base if available
                    if (has_projected_base)
                        out.write("export struct % : %\n{\n", c->name, base_cpp);
                    else
                        out.write("export struct %\n{\n", c->name);

                    out.write("    static constexpr std::wstring_view class_name() noexcept { return L\"%\"; }\n", c->full_name());
                    if (!has_projected_base)
                    {
                        out.write("    winrt::com_ptr<void> m_unknown;\n");
                        out.write("    mutable winrt::impl::iface_cache m_cache;\n\n");
                        out.write("    static % from_abi(void* ptr) noexcept { % result; result.m_unknown = winrt::com_ptr<void>::attach(ptr); return result; }\n", c->name, c->name);
                        out.write("    void* get_abi() const noexcept { return m_unknown.get(); }\n\n");
                        out.write("    static % wrap_borrowed(void* ptr) noexcept { if (ptr) { auto* vt = *reinterpret_cast<void* const**>(ptr); reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vt[1])(ptr); } return from_abi(ptr); }\n\n", c->name);
                        out.write("    template <typename I>\n    void* iface() const {\n        void* result = m_cache.get(winrt::iid_of<I>, m_unknown.get());\n        if (!result) throw winrt::hresult_error(winrt::hr_no_interface);\n        return result;\n    }\n\n");
                        out.write("    void* addref_borrowed() const noexcept\n    {\n        if (m_unknown.get())\n        {\n            auto* vt = *reinterpret_cast<void* const**>(m_unknown.get());\n            reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vt[1])(m_unknown.get());\n        }\n        return m_unknown.get();\n    }\n\n");
                        out.write("    operator winrt::IInspectable() const noexcept { return winrt::IInspectable(addref_borrowed()); }\n\n");
                    }
                    out.write("    %() { m_unknown = winrt::com_ptr<void>::attach(winrt::impl::create_instance(class_name())); }\n\n", c->name);
                    out.write("    using implements_type = %;\n", c->name);
                    out.write("    using default_interface = winrt::IInspectable;\n\n");

                    // No generic property forwarding — properties are emitted below from interfaces

                    // Emit dependency property stubs for app component types.
                    // These are properties used by view modules but not present in .winmd
                    // (e.g. ControlExample.Prefix, ColorTile.ColorBrushName).
                    // We scan the output folder's view .ixx files to discover which
                    // properties are called on each component type.
                    if (settings.component && winuic::g_project_nss.count(c->ns))
                    {
                        static std::unordered_map<std::string, std::set<std::string>> comp_props_cache;
                        static bool cache_built = false;
                        if (!cache_built)
                        {
                            cache_built = true;
                            for (auto const& entry : std::filesystem::directory_iterator(settings.output_folder))
                            {
                                if (!entry.is_regular_file() || entry.path().extension() != ".ixx") continue;
                                std::ifstream ifs(entry.path());
                                std::string line;
                                while (std::getline(ifs, line))
                                {
                                    for_each_prop_call(line, [&](std::string const& type_in, std::string const& prop_in)
                                    {
                                        std::string type = type_in;
                                        std::string prop = prop_in;
                                        // Locals are named <Type>_<n> - the element type is the
                                        // prefix before the trailing _digits suffix.
                                        auto us = type.find_last_of('_');
                                        if (us != std::string::npos && us + 1 < type.size())
                                        {
                                            bool all_digits = true;
                                            for (std::size_t di = us + 1; di < type.size(); ++di)
                                                if (!std::isdigit(static_cast<unsigned char>(type[di]))) { all_digits = false; break; }
                                            if (all_digits) type.resize(us);
                                        }
                                        // Check if this type is a component we care about
                                        if (winuic::g_project_nss.count(winuic::resolve_type_ns(type)))
                                            comp_props_cache[type].insert(prop);
                                    });
                                }
                            }
                        }
                        auto props_it = comp_props_cache.find(c->name);
                        if (props_it != comp_props_cache.end())
                        {
                            // Collect already-emitted method names
                            std::set<std::string, std::less<>> existing;
                            for (auto const* iface : c->interfaces)
                            {
                                if (!iface || !iface->is_interface()) continue;
                                auto methods = flattened_methods(*iface, metadata);
                                for (auto const* m : methods)
                                    existing.insert(m->name);
                            }
                            for (auto const& prop : props_it->second)
                            {
                                if (existing.count(prop) || existing.count("get_" + prop) || existing.count("put_" + prop))
                                    continue;
                                // Members the projected base chain already provides are
                                // forwarded by the typed base-forwarder pass below (get_
                                // /put_ resolved through the base). A stub here (hstring
                                // or add_) would duplicate those declarations - the
                                // Content case C2556s (hstring vs UIElement overloads
                                // differing only by return type). Keep stubs only for
                                // members that exist nowhere in the metadata chain.
                                bool on_base = false;
                                {
                                    winmd::type_info const* bl = c->base;
                                    int bh = 0;
                                    while (bl && bh++ < 10)
                                    {
                                        if (!is_component_type(*bl))
                                        {
                                            for (auto const* liface : bl->interfaces)
                                            {
                                                if (!liface || !liface->is_interface()) continue;
                                                auto lm = flattened_methods(*liface, metadata);
                                                bool hit = false;
                                                for (auto const* mm : lm)
                                                {
                                                    if (mm->is_static) continue;
                                                    if (mm->name == "get_" + prop || mm->name == "put_" + prop
                                                        || mm->name == "add_" + prop || mm->name == "remove_" + prop)
                                                    { hit = true; break; }
                                                }
                                                if (hit) { on_base = true; break; }
                                            }
                                        }
                                        if (on_base) break;
                                        bl = bl->base;
                                    }
                                }
                                if (on_base) continue;
                                // Event wiring on component classes (pages call
                                // Type.add_<Event>(lambda)): real component structs get
                                // no event accessors from the winmd, so emit a lambda-
                                // swallowing add_<Event> forwarder (compile-equivalent;
                                // runtime wiring is the host layer's job).
                                if (prop.rfind("add_", 0) == 0)
                                {
                                    out.write("    template<class H> winrt::event_token %(H const&) const { return {}; }\n", prop);
                                    continue;
                                }
                                // Emit as hstring property (most dependency props are strings)
                                std::string pname = prop;
                                if (!pname.empty()) pname[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(pname[0])));
                                out.write("    winrt::hstring %() const { return {}; }\n", pname);
                                out.write("    void %(winrt::hstring const& value) const {}\n", pname);
                            }
                        }
                    }


                    // Raw methods + ABI bodies + convenience wrappers for own interfaces
                    std::set<std::string, std::less<>> emitted_keys;
                    std::set<std::string, std::less<>> wrapper_emitted;
                    for (auto const* iface : c->interfaces)
                    {
                        if (!iface || !iface->is_interface()) continue;
                        std::string iname = iface->name;
                        if (iname.find("Static") != std::string::npos ||
                            iname.find("Factory") != std::string::npos) continue;
                        auto methods = flattened_methods(*iface, metadata);
                        auto iface_cpp = type_cpp_name(*iface);
                        std::string qual = type_cpp_name(*c);
                        for (std::size_t i = 0; i < methods.size(); ++i)
                        {
                            auto const& m = *methods[i];
                            if (m.is_static) continue;
                            if (!emitted_keys.insert(m.name + "|" + mapper.map(m.ret)).second) continue;
                            // Emit raw method declaration
                            out.write("    %;\n", method_sig(m, mapper, true));
                            // Emit ABI body out-of-line
                            writer b;
                            b.write("        auto* p = iface<%>();\n", iface_cpp);
                            b.write("        if (!p) throw winrt::hresult_error(winrt::hr_no_interface);\n");
                            b.write("%", abi_call_body(m, "p", 6 + i, mapper, metadata, "        "));
                            component_bodies += out_of_line(*c, metadata, method_sig(m, mapper, true, qual), b.str());
                            std::string name = m.name;
                            // Convenience wrapper: put_X -> Property(val)
                            if (name.starts_with("put_") && name.size() > 4 && m.params.size() == 1)
                            {
                                std::string prop = name.substr(4);
                                if (wrapper_emitted.insert("put_" + prop).second)
                                {
                                    std::string param_type = mapper.map(m.params[0].value);
                                    if (param_type == "winrt::IInspectable")
                                    {
                                        out.write("    void %(const winrt::hstring& value) const { put_%(winrt::IInspectable(winrt::box_value(value))); }\n", prop, prop);
                                        out.write("    void %(const winrt::IInspectable& value) const { put_%(value); }\n", prop, prop);
                                    }
                                    else
                                        out.write("    void %(const %& value) const { put_%(value); }\n", prop, param_type, prop);
                                }
                            }
                            // Convenience wrapper: get_X -> Property()
                            if (name.starts_with("get_") && name.size() > 4 && m.params.empty())
                            {
                                std::string prop = name.substr(4);
                                if (wrapper_emitted.insert("get_" + prop).second)
                                {
                                    std::string ret = mapper.map(m.ret);
                                    out.write("    % %() const { return get_%(); }\n", ret, prop, prop);
                                }
                            }
                            // Convenience wrapper: add_X -> event X(handler)
                            if (name.starts_with("add_") && name.size() > 4 && m.params.size() == 1)
                            {
                                std::string evname = name.substr(4);
                                if (wrapper_emitted.insert("add_" + evname).second)
                                {
                                    std::string param_type = mapper.map(m.params[0].value);
                                    std::string ret_mapped = mapper.map(m.ret);
                                    out.write("    % %(const %& handler) const { return add_%(handler); }\n", ret_mapped, evname, param_type, evname);
                                }
                            }
                        }
                    }

                    // Forward base-class members used by the emitted view
                    // modules. MSVC C++ modules cannot resolve members that a
                    // component class inherits from its platform base when the
                    // class is exported from another module (C2039 on Content
                    // etc. — verified with an isolated probe), so re-declare
                    // the exact members the views call and forward them to the
                    // base subobject. That call is resolved inside this module,
                    // where the base type is complete, so it compiles cleanly.
                    if (has_projected_base && winuic::g_project_nss.count(c->ns))
                    {
                        static std::unordered_map<std::string, std::set<std::string, std::less<>>, std::hash<std::string>> comp_used_members;
                        static bool used_cache_built = false;
                        if (!used_cache_built)
                        {
                            used_cache_built = true;
                            for (auto const& entry : std::filesystem::directory_iterator(settings.output_folder))
                            {
                                if (!entry.is_regular_file() || entry.path().extension() != ".ixx") continue;
                                std::ifstream ifs(entry.path());
                                std::string line;
                                while (std::getline(ifs, line))
                                {
                                    for_each_call_pattern(line, [&](std::string const& type_in, std::string const& member_in)
                                    {
                                        comp_used_members[type_in].insert(member_in);
                                    });
                                }
                            }
                        }
                        auto used_it = comp_used_members.find(c->name);
                        if (used_it != comp_used_members.end())
                        {
                            for (auto const& member : used_it->second)
                            {
                                if (emitted_keys.count(member) || emitted_keys.count("get_" + member) ||
                                    emitted_keys.count("put_" + member) || emitted_keys.count("add_" + member) ||
                                    emitted_keys.count("remove_" + member))
                                    continue;
                                if (!wrapper_emitted.insert("fwd_" + member).second) continue;

                                struct raw_ref
                                {
                                    winmd::method_info const* m = nullptr;
                                    winmd::type_info const* level = nullptr;
                                };
                                raw_ref getter, setter, adder, remover;
                                winmd::type_info const* level = c->base;
                                int hops = 0;
                                while (level && hops++ < 10)
                                {
                                    if (!is_component_type(*level))
                                    {
                                        for (auto const* liface : level->interfaces)
                                        {
                                            if (!liface || !liface->is_interface()) continue;
                                            auto lm = flattened_methods(*liface, metadata);
                                            for (auto const* mm : lm)
                                            {
                                                if (mm->is_static) continue;
                                                if (mm->name == "get_" + member && mm->params.empty() && !getter.m) { getter.m = mm; getter.level = level; }
                                                else if (mm->name == "put_" + member && mm->params.size() == 1 && !setter.m) { setter.m = mm; setter.level = level; }
                                                else if (mm->name == "add_" + member && mm->params.size() == 1 && !adder.m) { adder.m = mm; adder.level = level; }
                                                else if (mm->name == "remove_" + member && mm->params.size() == 1 && !remover.m) { remover.m = mm; remover.level = level; }
                                            }
                                        }
                                    }
                                    level = level->base;
                                }
                                if (setter.m)
                                {
                                    std::string base_cpp = type_cpp_name(*setter.level);
                                    std::string param_type = mapper.map(setter.m->params[0].value);
                                    if (param_type == "winrt::IInspectable")
                                    {
                                        out.write("    void %(const winrt::hstring& value) const { static_cast<% const&>(*this).put_%(winrt::IInspectable(winrt::box_value(value))); }\n", member, base_cpp, member);
                                        out.write("    void %(const winrt::IInspectable& value) const { static_cast<% const&>(*this).put_%(value); }\n", member, base_cpp, member);
                                    }
                                    else
                                    {
                                        out.write("    void %(const %& value) const { static_cast<% const&>(*this).put_%(value); }\n", member, param_type, base_cpp, member);
                                    }
                                }
                                if (getter.m)
                                {
                                    std::string base_cpp = type_cpp_name(*getter.level);
                                    std::string ret_mapped = mapper.map(getter.m->ret);
                                    out.write("    % %() const { return static_cast<% const&>(*this).get_%(); }\n", ret_mapped, member, base_cpp, member);
                                }
                                if (adder.m)
                                {
                                    std::string base_cpp = type_cpp_name(*adder.level);
                                    std::string param_type = mapper.map(adder.m->params[0].value);
                                    std::string ret_mapped = mapper.map(adder.m->ret);
                                    out.write("    % %(const %& handler) const { return static_cast<% const&>(*this).add_%(handler); }\n", ret_mapped, member, param_type, base_cpp, member);
                                }
                                if (remover.m)
                                {
                                    std::string base_cpp = type_cpp_name(*remover.level);
                                    std::string param_type = mapper.map(remover.m->params[0].value);
                                    out.write("    void remove_%(const %& token) const { static_cast<% const&>(*this).remove_%(token); }\n", member, param_type, base_cpp, member);
                                }
                            }
                        }
                    }

                    out.write("};\n\n");
                }
                out.write("}\n");
            }
            if (!component_bodies.empty())
            {
                // statics/factory method bodies that reference a component
                // class: fully qualified, so module scope is fine; the
                // class projections above are complete by now
                out.write("%", component_bodies);
            }
            write_output_file(folder / (settings.component_name + ".ixx"), out.str());
            } // component module clobber guard else
        }

        // Remove stale module files from earlier generations (different input
        // set / namespace grouping) that are not part of this run — they would
        // otherwise be compiled/imported by mistake and slow down the build.
        // Runs after all output (incl. the component module) has been emitted.
        // Only in selective project mode (views present: -winui,
        // -winui_no_compile, or component runs); full SDK projections keep the
        // documented never-delete in-place regeneration behavior. Only touches
        // files whose module name follows this generator's naming patterns, so
        // user-authored modules are never deleted.
        if (!needed_modules.empty())
        {
            std::vector<std::filesystem::path> stale;
            for (auto const& entry : std::filesystem::directory_iterator(output_folder))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".ixx") continue;
                std::string stem = entry.path().stem().string();
                if (emitted_this_run.contains(stem)) continue;
                bool generator_named = stem == "winrt_base" || stem == "main"
                    || stem.starts_with("winrt.")
                    || stem.starts_with("Windows.Win32.") || stem.starts_with("Windows.Wdk.")
                    || stem == "Windows.Win32" || stem == "Windows.Wdk"
                    || (!settings.component_name.empty()
                        && (stem == settings.component_name || stem.starts_with(settings.component_name + ".")));
                if (!generator_named) continue;
                // Double-check the module declaration matches the file name
                // before removing anything.
                std::ifstream ifs(entry.path());
                std::string line;
                bool owned = false;
                int scanned = 0;
                std::string needle = "export module " + stem + ";";
                while (std::getline(ifs, line) && scanned++ < 200)
                {
                    if (line.find(needle) != std::string::npos) { owned = true; break; }
                }
                if (owned) stale.push_back(entry.path());
            }
            for (auto const& path : stale)
            {
                std::error_code ec;
                std::filesystem::remove(path, ec);
                if (settings.verbose && !ec)
                    std::printf("cppwinc: removed stale module %s\n", path.filename().string().c_str());
            }
        }

        // Project-mode prune (only for -winui / -winui_no_compile): keep only
        // winrt.* modules actually reachable from the project's own files
        // (view modules / component module / main) through their real import
        // statements.  The metadata-dependency expansion above is deliberately
        // broad (a generated module must be able to compile against every type
        // it mentions), but a simple project like MetaScrub must not drag in
        // the whole SDK projection: files no project module imports are
        // deleted here.
        if (settings.winui)
        {
            std::set<std::string, std::less<>> seeds;
            for (auto const& stem : emitted_this_run)
            {
                if (!stem.starts_with("winrt.") && stem != "winrt_base")
                    seeds.insert(stem);
            }
            if (!seeds.empty())
            {
                std::regex import_re(R"(^\s*(?:export\s+)?import\s+([\w.]+)\s*;)", std::regex::ECMAScript);
                std::set<std::string, std::less<>> reachable;
                std::vector<std::string> queue(seeds.begin(), seeds.end());
                // Map stem -> on-disk path once
                std::map<std::string, std::filesystem::path, std::less<>> stem_path;
                for (auto const& entry : std::filesystem::directory_iterator(output_folder))
                {
                    if (!entry.is_regular_file() || entry.path().extension() != ".ixx") continue;
                    stem_path[entry.path().stem().string()] = entry.path();
                }
                while (!queue.empty())
                {
                    std::string mod = queue.back();
                    queue.pop_back();
                    if (!reachable.insert(mod).second) continue;
                    auto pit = stem_path.find(mod);
                    if (pit == stem_path.end()) continue;
                    std::ifstream ifs(pit->second);
                    std::string line;
                    while (std::getline(ifs, line))
                    {
                        std::smatch m;
                        if (std::regex_match(line, m, import_re))
                        {
                            std::string dep = m[1].str();
                            if (dep == "std") continue;
                            if (stem_path.count(dep)) queue.push_back(dep);
                        }
                    }
                }
                // Delete generator winrt.* modules the project never imports.
                std::vector<std::filesystem::path> extra;
                for (auto const& entry : std::filesystem::directory_iterator(output_folder))
                {
                    if (!entry.is_regular_file() || entry.path().extension() != ".ixx") continue;
                    std::string stem = entry.path().stem().string();
                    bool generator_winrt = stem == "winrt_base" || stem.starts_with("winrt.");
                    if (generator_winrt && !reachable.contains(stem)) extra.push_back(entry.path());
                }
                for (auto const& path : extra)
                {
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                    if (settings.verbose && !ec)
                        std::printf("cppwinc: pruned unneeded module %s\n", path.filename().string().c_str());
                }
            }
        }

        if (settings.verbose)
        {
            std::printf(" done.\n");
        }

        // -std together with a projection: build the std named module
        // into the output folder (after generation, so it never shares a
        // folder with the stale-module cleanup)
        if (args.exists("std"))
        {
            std::filesystem::path std_out = args.value("std");
            if (std_out.empty())
            {
                std_out = settings.output_folder;
            }
            if (!build_std_module(std_out))
            {
                result = 1;
            }
        }

        // -winui / -compile: compile all generated .ixx modules, link the
        // app exe (main.ixx bootstrap + views + user .cpp) and a driver .sys
        // when a DriverEntry source is present, via compile_project_folder.
        phase("module emission done");

        // -winui copies the winuic-produced resources.pri (sits next to the
        // app .winmd) into the output folder so the exe loads its UI layout,
        // and puts the bootstrap DLL beside the exe so the unpackaged app
        // can call MddBootstrapInitialize2 at startup.
        if (settings.winui)
        {
        copy_winui_pri();
        copy_winui_assets();
        copy_winui_bootstrap_dll();
        }

        // -compile: same compile+link pass as -winui, without WASDK discovery —
        // for winrt / win32 / wdk projections and any custom winmd input.
        bool do_compile = settings.winui
            ? !settings.winui_no_compile
            : settings.compile;
        if (do_compile)
        {
            std::filesystem::path proj_folder(settings.output_folder);
            // Final binaries land in the -compile/-winui path-switch folder
            // when one was given (only the exe/sys/dll go there), else in
            // the -output folder itself.
            std::string final_out = settings.final_output.empty()
                ? settings.output_folder : settings.final_output;
            if (compile_project_folder(proj_folder, settings.component_name, final_out) != 0)
                result = 1;
        }
    }
    catch (usage_exception const&)
    {
        print_usage();
    }
    catch (std::exception const& e)
    {
        std::printf("cppwinc : error %s\n", e.what());
        result = 1;
    }
    return result;
}

// locate_wasdk_bootstrap_lib — find Microsoft.WindowsAppRuntime.Bootstrap.lib
// in the local NuGet cache (ships with the WindowsAppSDK packages). The
// bootstrap lib provides MddBootstrapInitialize2 for the unpackaged app exe.
std::string locate_wasdk_bootstrap_lib(bool verbose)
{
    std::filesystem::path pkgs = nuget_packages_root();
    if (pkgs.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_directory(pkgs, ec)) return {};
    // Collect every x64 candidate, then return the one from the highest
    // package version — old 1.0.x libs lack MddBootstrapInitialize2 (1.1+),
    // so taking the first hit (alphabetical = 1.0.0) poisons the link.
    std::vector<std::pair<std::array<unsigned long, 4>, std::string>> candidates;
    for (auto const& pkg : std::filesystem::directory_iterator(pkgs, ec))
    {
        auto name = pkg.path().filename().wstring();
        if (name.rfind(L"microsoft.windowsappsdk", 0) != 0 && name.rfind(L"microsoft.windowsappruntime", 0) != 0)
            continue;
        for (auto const& ver : std::filesystem::directory_iterator(pkg.path(), ec))
        {
            if (!ver.is_directory()) continue;
            for (auto const& f : std::filesystem::recursive_directory_iterator(ver.path(),
                    std::filesystem::directory_options::skip_permission_denied, ec))
            {
                // The packages ship arm64/x86/x64 flavors — a mismatched
                // machine type poisons the link (LNK4272 + unresolved
                // MddBootstrap*). Only the x64 flavor links an x64 exe.
                auto p = f.path().wstring();
                if (!f.is_regular_file() || f.path().filename() != L"Microsoft.WindowsAppRuntime.Bootstrap.lib")
                    continue;
                if (p.find(L"x64") == std::wstring::npos || p.find(L"arm") != std::wstring::npos)
                    continue;
                std::array<unsigned long, 4> v = { 0, 0, 0, 0 };
                std::wstring vstr = ver.path().filename().wstring();
                int idx = 0;
                unsigned long cur = 0;
                bool any = false;
                for (wchar_t c : vstr)
                {
                    if (c >= L'0' && c <= L'9') { cur = cur * 10 + (c - L'0'); any = true; }
                    else { if (any && idx < 4) v[idx++] = cur; cur = 0; any = false; }
                }
                if (any && idx < 4) v[idx] = cur;
                candidates.push_back({ v, f.path().string() });
            }
        }
    }
    if (!candidates.empty())
    {
        auto best = std::max_element(candidates.begin(), candidates.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });
        return best->second;
    }
    if (verbose) std::printf("  (no Microsoft.WindowsAppRuntime.Bootstrap.lib in NuGet cache)\n");
    return {};
}

// locate_wasdk_bootstrap_dll — find Microsoft.WindowsAppRuntime.Bootstrap.dll
// (x64) in the local NuGet cache. The unpackaged exe calls
// MddBootstrapInitialize2 at startup, which loads this DLL — without a copy
// next to the exe the app fails immediately on machines without the runtime
// installed system-wide. Same highest-version-wins rule as the .lib locator.
std::string locate_wasdk_bootstrap_dll(bool verbose)
{
    std::filesystem::path pkgs = nuget_packages_root();
    if (pkgs.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_directory(pkgs, ec)) return {};
    std::vector<std::pair<std::array<unsigned long, 4>, std::string>> candidates;
    for (auto const& pkg : std::filesystem::directory_iterator(pkgs, ec))
    {
        auto name = pkg.path().filename().wstring();
        if (name.rfind(L"microsoft.windowsappsdk", 0) != 0 && name.rfind(L"microsoft.windowsappruntime", 0) != 0)
            continue;
        for (auto const& ver : std::filesystem::directory_iterator(pkg.path(), ec))
        {
            if (!ver.is_directory()) continue;
            for (auto const& f : std::filesystem::recursive_directory_iterator(ver.path(),
                    std::filesystem::directory_options::skip_permission_denied, ec))
            {
                // x64 flavor only — the produced exe is x64 (same rule as the lib).
                auto p = f.path().wstring();
                if (!f.is_regular_file() || f.path().filename() != L"Microsoft.WindowsAppRuntime.Bootstrap.dll")
                    continue;
                if (p.find(L"x64") == std::wstring::npos || p.find(L"arm") != std::wstring::npos)
                    continue;
                std::array<unsigned long, 4> v = { 0, 0, 0, 0 };
                std::wstring vstr = ver.path().filename().wstring();
                int idx = 0;
                unsigned long cur = 0;
                bool any = false;
                for (wchar_t c : vstr)
                {
                    if (c >= L'0' && c <= L'9') { cur = cur * 10 + (c - L'0'); any = true; }
                    else { if (any && idx < 4) v[idx++] = cur; cur = 0; any = false; }
                }
                if (any && idx < 4) v[idx] = cur;
                candidates.push_back({ v, f.path().string() });
            }
        }
    }
    if (!candidates.empty())
    {
        auto best = std::max_element(candidates.begin(), candidates.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });
        return best->second;
    }
    if (verbose) std::printf("  (no Microsoft.WindowsAppRuntime.Bootstrap.dll in NuGet cache)\n");
    return {};
}

// Runtime folder for the -winui companions: wherever the linked exe lands
// (-winui/-compile path switch, else -output). resources.pri / <exe>.pri /
// Assets / the bootstrap DLL are only found at runtime NEXT TO the exe.
static std::string winui_runtime_dir()
{
    std::string dir = settings.final_output.empty() ? settings.output_folder : settings.final_output;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// copy_winui_bootstrap_dll — put Microsoft.WindowsAppRuntime.Bootstrap.dll
// beside the produced app exe so MddBootstrapInitialize2 resolves at startup
// on machines without a system-wide Windows App Runtime install.
void copy_winui_bootstrap_dll()
{
    std::string dll = locate_wasdk_bootstrap_dll(false);
    if (dll.empty())
    {
        std::printf("cppwinc : warning Microsoft.WindowsAppRuntime.Bootstrap.dll not found in NuGet cache;"
                    " copy it next to the exe or install the Windows App Runtime\n");
        return;
    }
    auto dst = std::filesystem::path(winui_runtime_dir()) / L"Microsoft.WindowsAppRuntime.Bootstrap.dll";
    // Always overwrite: a stale lower-version DLL left in the output folder
    // breaks MddBootstrapInitialize2 (0x80670016) — the located source is
    // always the highest-version x64 DLL from the NuGet cache.
    std::error_code ec;
    std::filesystem::copy_file(dll, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec)
        std::printf("-winui: Microsoft.WindowsAppRuntime.Bootstrap.dll copied next to the exe\n");
    else
        std::printf("cppwinc : warning could not copy Microsoft.WindowsAppRuntime.Bootstrap.dll: %s\n", ec.message().c_str());
}

// copy_winui_pri — winuic writes resources.pri next to the app .winmd; the
// generated app needs it beside the exe, so -winui copies it into the
// output folder (a newer local copy is never overwritten).
// MRT Core in an unpackaged desktop app loads the package index from
// "<ExeName>.pri" next to the exe (the exe name = output folder name,
// matching the link step); "resources.pri" alone is never found, which
// makes every resource lookup fail with 0x80070003. Both names are
// written so unpackaged and packaged-style consumers both work.
void copy_winui_pri()
{
    for (auto const& in : settings.input)
    {
        if (!std::filesystem::is_regular_file(in)) continue;
        auto pri = in.parent_path() / L"resources.pri";
        if (!std::filesystem::exists(pri)) continue;
        // exe base name = component_name (the app winmd stem, same rule as
        // the link step); fall back to the output folder name
        std::wstring exe_name = std::wstring(settings.component_name.begin(), settings.component_name.end());
        if (exe_name.empty())
        {
            std::filesystem::path out_dir = std::filesystem::path(settings.output_folder).lexically_normal();
            exe_name = out_dir.filename().wstring();
            while (!exe_name.empty() && (exe_name.back() == L'/' || exe_name.back() == L'\\'))
                exe_name.pop_back();
        }
        std::error_code ec;
        auto dst_res = std::filesystem::path(winui_runtime_dir()) / L"resources.pri";
        std::filesystem::copy_file(pri, dst_res, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec)
            std::printf("-winui: resources.pri copied from %s\n", pri.string().c_str());
        else
            std::printf("cppwinc : warning could not copy resources.pri: %s\n", ec.message().c_str());
        if (!exe_name.empty())
        {
            auto dst_exe = std::filesystem::path(winui_runtime_dir()) / (exe_name + L".pri");
            std::error_code ec2;
            std::filesystem::copy_file(pri, dst_exe, std::filesystem::copy_options::overwrite_existing, ec2);
            if (!ec2)
                std::printf("-winui: %s.pri copied (MRT Core looks for <ExeName>.pri)\n", to_utf8(exe_name).c_str());
            else
                std::printf("cppwinc : warning could not copy %s.pri: %s\n", to_utf8(exe_name).c_str(), ec2.message().c_str());
        }
        return;
    }
}

// copy_winui_assets — winuic emits the app's Assets folder next to the
// app .winmd (images, icons, and any other packaged payloads referenced by
// the .winui sources). The produced app needs those same files beside the
// exe (ms-appx:///Assets/... resolution), so -winui mirrors the whole
// folder into the output folder. Always overwrites so assets always match
// the winmd that produced this projection.
void copy_winui_assets()
{
    for (auto const& in : settings.input)
    {
        if (!std::filesystem::is_regular_file(in)) continue;
        auto src = in.parent_path() / L"Assets";
        if (!std::filesystem::is_directory(src)) continue;
        auto dst = std::filesystem::path(winui_runtime_dir()) / L"Assets";
        std::error_code ec;
        std::filesystem::copy(src, dst,
            std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive, ec);
        if (!ec)
            std::printf("-winui: Assets copied from %s\n", src.string().c_str());
        else
            std::printf("cppwinc : warning could not copy Assets: %s\n", ec.message().c_str());
        return;
    }
}

// compile_project_folder — the shared compile+link pass behind -winui and
// -compile (and the standalone `-compile <folder>` quick test).
// 1. compiles every .ixx module topologically, cached ifcs skipped, big
//    modules serialized, the rest across all cores;
// 2. compiles any .cpp app sources in the folder;
// 3. links the app <name>.exe when a main.ixx bootstrap is present
//    (-winui) and a kernel <name>.sys when a DriverEntry source exists
//    (-compile); the WDK module objects feed the driver link, everything
//    else feeds the app link.
int compile_project_folder(std::filesystem::path const& proj_folder, std::string const& app_name,
                           std::string const& final_out)
{
    int result = 0;
    std::filesystem::path toolchain = locate_msvc_toolchain();
    if (toolchain.empty())
    {
        std::printf("cppwinc : error could not locate the MSVC toolchain\n");
        return 1;
    }
                std::regex import_re(R"(^\s*(?:export\s+)?import\s+([\w.]+)\s*;)", std::regex::ECMAScript);

                // Collect all .ixx modules and parse imports
                struct mod_info { std::string name; std::vector<std::string> imports; };
                std::vector<mod_info> modules;
                for (auto const& entry : std::filesystem::directory_iterator(proj_folder))
                {
                    if (entry.path().extension() != ".ixx") continue;
                    mod_info mi;
                    mi.name = entry.path().stem().string();
                    std::ifstream ifs(entry.path());
                    std::string line;
                    while (std::getline(ifs, line))
                    {
                        std::smatch m;
                        if (std::regex_search(line, m, import_re))
                        {
                            std::string imp = m[1].str();
                            if (imp != mi.name)
                                mi.imports.push_back(std::move(imp));
                        }
                    }
                    modules.push_back(std::move(mi));
                }

                // Topological sort into a single ordered list
                std::unordered_map<std::string, std::size_t> name_to_idx;
                for (std::size_t i = 0; i < modules.size(); ++i)
                    name_to_idx[modules[i].name] = i;
                std::vector<std::string> ordered;
                std::unordered_set<std::string> compiled_set;
                std::size_t remaining = modules.size();
                while (remaining > 0)
                {
                    std::vector<std::string> level;
                    for (auto const& mi : modules)
                    {
                        if (compiled_set.contains(mi.name)) continue;
                        bool ready = true;
                        for (auto const& imp : mi.imports)
                            if (!compiled_set.contains(imp) && name_to_idx.contains(imp)) { ready = false; break; }
                        if (ready) level.push_back(mi.name);
                    }
                    if (level.empty())
                        for (auto const& mi : modules)
                            if (!compiled_set.contains(mi.name)) level.push_back(mi.name);
                    for (auto const& n : level) { compiled_set.insert(n); ordered.push_back(n); }
                    remaining -= level.size();
                }

                // Build include paths — same approach as build_std_module
                std::string toolchain_inc = (toolchain / "include").string();
                std::string toolchain_lib = (toolchain / "lib" / "x64").string();
                std::string sdk_inc, sdk_lib;
                {
                    std::filesystem::path kits;
                    if (const char* pf_x86 = std::getenv("ProgramFiles(x86)"))
                        kits = std::filesystem::path(pf_x86) / "Windows Kits" / "10" / "Include";
                    else if (const char* pf = std::getenv("ProgramFiles"))
                        kits = std::filesystem::path(pf) / "Windows Kits" / "10" / "Include";
                    std::error_code sec;
                    std::filesystem::path newest;
                    if (!kits.empty())
                        for (auto const& entry : std::filesystem::directory_iterator(kits, sec))
                        {
                            auto fname = entry.path().filename().string();
                            // Version dirs only (10.0.x.x) — same guard as
                            // build_std_module: 'wdf' must not win the compare.
                            if (entry.is_directory() && !fname.empty() &&
                                std::isdigit(static_cast<unsigned char>(fname.front())) &&
                                (newest.empty() || fname > newest.filename().string()))
                                newest = entry.path();
                        }
                    if (!newest.empty())
                    {
                        sdk_inc = newest.string();
                        sdk_lib = (newest.parent_path().parent_path() / "Lib" / newest.filename().string()).string();
                    }
                }
                // WASDK include
                std::string wasdk_inc;
                {
                    auto nugget = nuget_packages_root() / L"microsoft.windowsappsdk.foundation";
                    if (std::filesystem::is_directory(nugget))
                    {
                        std::filesystem::path latest_ver;
                        for (auto const& e : std::filesystem::directory_iterator(nugget))
                        {
                            if (!e.is_directory()) continue;
                            auto p = e.path().filename().string();
                            if (!p.empty() && std::isdigit(static_cast<unsigned char>(p.front())))
                                if (latest_ver.empty() || p > latest_ver.filename().string())
                                    latest_ver = e.path();
                        }
                        if (!latest_ver.empty()) wasdk_inc = (latest_ver / L"include").string();
                    }
                }

                std::filesystem::path out_clean = proj_folder.lexically_normal();
                auto out_str = out_clean.string();
                while (!out_str.empty() && (out_str.back() == '/' || out_str.back() == '\\'))
                    out_str.pop_back();
                std::filesystem::path obj_dir = std::filesystem::path(out_str) / "build" / "obj";
                std::filesystem::create_directories(obj_dir);

                // Auto-build std module into obj_dir if missing
                if (!std::filesystem::exists(obj_dir / "std.ifc"))
                {
                    if (!build_std_module(obj_dir))
                    {
                        std::printf("cppwinc : error failed to build std module into %s\n", obj_dir.string().c_str());
                        result = 1;
                    }
                }

                std::string abs_obj = std::filesystem::weakly_canonical(obj_dir).string();
                std::string cl_path = (toolchain / "bin" / "Hostx64" / "x64" / "cl.exe").string();
                std::string abs_ixx = std::filesystem::weakly_canonical(proj_folder).string();

                // Build INCLUDE and LIB strings for child processes
                std::string include_env = toolchain_inc;
                if (!sdk_inc.empty())
                    include_env += ";" + sdk_inc + "\\ucrt;" + sdk_inc + "\\shared;" + sdk_inc + "\\um;" + sdk_inc + "\\winrt";
                if (!wasdk_inc.empty())
                    include_env += ";" + wasdk_inc;
                std::string lib_env = toolchain_lib;
                if (!sdk_lib.empty())
                    lib_env += ";" + sdk_lib + "\\um\\x64;" + sdk_lib + "\\ucrt\\x64";                // Build per-module compile commands (cached modules skipped)
                // Build env prefix for each child process
                std::string env_prefix = "set \"INCLUDE=" + include_env + "\" & set \"LIB=" + lib_env + "\" & cd /d \"" + abs_obj + "\" & ";

                // Use parallel vectors instead of local struct (MSVC C++20 modules
                // forbid local types as std::vector template arguments)
                std::vector<std::string> cj_names;
                std::vector<std::string> cj_cmds;
                auto is_sdk_mod = [](std::string_view name) {
                    // winrt_base is NOT here: it is regenerated by this
                    // compiler on every run (its emission lives in
                    // strings_data.ixx and changes whenever the runtime
                    // template changes). Trusting its existing .ifc as
                    // "precompiled platform code" shadowed every winrt_base
                    // source change behind a stale interface — all view
                    // modules import winrt_base, so one stale ifc failed the
                    // whole gallery with errors the fixes had already solved.
                    //
                    // winrt.* is NOT here either: although its own interfaces
                    // are stable, each winrt.* .obj INSTANTIATES inline
                    // winrt_base functions it calls (create_instance,
                    // register_local_factory, check_hresult, ...). A cached
                    // pre-change .obj carries COMDAT copies of the OLD bodies,
                    // and the linker keeps the first copy it sees (winrt.*
                    // sorts before WinUIGallery.*) — silently shadowing the
                    // new runtime for the whole exe. They must recompile
                    // whenever winrt_base does (the transitive-poisoning pass
                    // below handles that once they are out of this set).
                    // std / Windows.Win32.* genuinely never import winrt_base:
                    // their artifacts are stable across runs.
                    return name == "std" || name.starts_with("Windows.Win32");
                };

                // Make-style transitive invalidation. A module whose .ifc
                // regenerates poisons every consumer that transitively
                // imports it: inline class-statics (Grid::SetRowSpan &
                // friends) are emitted as COMDAT in EACH consuming TU, and
                // the linker keeps the first copy it sees. A consumer whose
                // .obj predates an imported module's regenerated ifc would
                // keep linking stale ABI code silently. Expand the stale
                // set through the import closure and drop those artifacts
                // up front so the loop below recompiles them.
                std::unordered_set<std::string> stale_set;
                {
                    for (auto const& n : ordered)
                    {
                        auto ixx = proj_folder / (n + ".ixx");
                        auto ifc = obj_dir / (n + ".ifc");
                        if (!std::filesystem::exists(ifc))
                        {
                            stale_set.insert(n);
                            continue;
                        }
                        // An SDK module with a valid .ifc (>=128) and .obj is pre-compiled platform projection;
                        // never mark it stale due to mtime differences. HOWEVER, a
                        // source whose .ixx was REGENERATED this run (ixx newer
                        // than ifc) must still rebuild — the stale .ifc carries
                        // the old type set (e.g. closure-seeded
                        // CollectionViewSource/XamlUICommand missing) and every
                        // dependent compiles against phantom declarations.
                        if (is_sdk_mod(n))
                        {
                            std::error_code sec, mec;
                            auto ifc_sz = std::filesystem::file_size(ifc, sec);
                            auto src_t2 = std::filesystem::last_write_time(ixx, mec);
                            auto ifc_t2 = std::filesystem::last_write_time(ifc, mec);
                            bool src_regen = !mec && !sec && ifc_sz >= 128 && src_t2 > ifc_t2;
                            if (ifc_sz >= 128 && std::filesystem::exists(obj_dir / (n + ".obj")) && !src_regen)
                                continue;
                        }
                        std::error_code fec;
                        auto src_t = std::filesystem::last_write_time(ixx, fec);
                        auto ifc_t = std::filesystem::last_write_time(ifc, fec);
                        if (fec || src_t > ifc_t)
                        {
                            stale_set.insert(n);
                            continue;
                        }
                        std::error_code sec;
                        auto ifc_sz = std::filesystem::file_size(ifc, sec);
                        if (!sec && ifc_sz < 128) stale_set.insert(n);
                    }
                    std::unordered_map<std::string, std::vector<std::string> const*> imp_map0;
                    for (std::size_t mi = 0; mi < modules.size(); ++mi)
                        imp_map0[modules[mi].name] = &modules[mi].imports;
                    bool grew = true;
                    while (grew)
                    {
                        grew = false;
                        for (auto const& n : ordered)
                        {
                            if (stale_set.contains(n)) continue;
                            // Precompiled SDK modules with valid .ifc and .obj should not be poisoned transitively
                            if (is_sdk_mod(n) && std::filesystem::exists(obj_dir / (n + ".ifc")) && std::filesystem::exists(obj_dir / (n + ".obj")))
                            {
                                std::error_code sec;
                                if (std::filesystem::file_size(obj_dir / (n + ".ifc"), sec) >= 128)
                                    continue;
                            }
                            auto it = imp_map0.find(n);
                            if (it == imp_map0.end()) continue;
                            std::unordered_set<std::string> visited;
                            std::vector<std::string> stack{n};
                            bool dirty = false;
                            while (!stack.empty() && !dirty)
                            {
                                std::string cur = stack.back();
                                stack.pop_back();
                                auto iit = imp_map0.find(cur);
                                if (iit == imp_map0.end()) continue;
                                for (auto const& imp : *iit->second)
                                {
                                    if (imp == cur) continue;
                                    if (stale_set.contains(imp)) { dirty = true; break; }
                                    if (visited.insert(imp).second) stack.push_back(imp);
                                }
                            }
                            if (dirty)
                            {
                                stale_set.insert(n);
                                grew = true;
                            }
                        }
                    }
                    for (auto const& n : stale_set)
                    {
                        // Protect precompiled SDK modules from deletion if they have valid artifacts
                        if (is_sdk_mod(n))
                        {
                            std::error_code sec;
                            auto ifc = obj_dir / (n + ".ifc");
                            if (std::filesystem::exists(ifc) && std::filesystem::file_size(ifc, sec) >= 128 && std::filesystem::exists(obj_dir / (n + ".obj")))
                                continue;
                        }
                        std::error_code dec;
                        std::filesystem::remove(obj_dir / (n + ".ifc"), dec);
                        std::filesystem::remove(obj_dir / (n + ".obj"), dec);
                    }
                    if (settings.verbose)
                        std::printf("  invalidating %zu modules (import closure of changed sources)\n", stale_set.size());
                }

                for (auto const& n : ordered)
                {
                    auto ixx = proj_folder / (n + ".ixx");
                    auto ifc = obj_dir / (n + ".ifc");
                    if (std::filesystem::exists(ifc))
                    {
                        // Only trust a cached ifc when it is newer than the
                        // module source AND looks like a real compile product.
                        // A cl.exe killed mid-compile (out of memory while
                        // several huge modules built in parallel) leaves a
                        // 1-byte .ifc behind; trusting it poisons every
                        // dependent module with C2230 "could not find module".
                        // A regenerated module (content-aware writes refresh
                        // mtime when bytes change) must be recompiled even
                        // though its old ifc still exists.
                        bool fresh = true;
                        std::error_code fec;
                        auto src_t = std::filesystem::last_write_time(ixx, fec);
                        auto ifc_t = std::filesystem::last_write_time(ifc, fec);
                        if (!fec && src_t > ifc_t) fresh = false;
                        if (fresh)
                        {
                            std::error_code sec;
                            auto ifc_sz = std::filesystem::file_size(ifc, sec);
                            // A killed cl.exe leaves a 1-byte .ifc. Legit thin
                            // re-export shells (P-parts with no own types) can
                            // be a few hundred bytes, so only distrust tiny ones.
                            if (!sec && ifc_sz < 128) fresh = false;
                        }
                        if (fresh)
                        {
                            // The cache pair is (.ifc, .obj): cl.exe killed
                            // between the two writes leaves a valid .ifc with
                            // NO .obj - trusting it starves the link with a
                            // wall of unresolved externals.
                            if (!std::filesystem::exists(obj_dir / (n + ".obj")))
                            {
                                fresh = false;
                                if (settings.verbose) std::printf("  [%s] recompiling (.ifc cached but .obj missing)\n", n.c_str());
                            }
                        }
                        // Precompiled SDK module with valid .ifc and .obj is always fresh
                        // — unless its .ixx was regenerated this run (same rule
                        // as the stale-set pass above: the cached .ifc then
                        // describes the OLD type set and must be rebuilt).
                        if (!fresh && is_sdk_mod(n))
                        {
                            std::error_code sec, mec;
                            auto ifc_sz = std::filesystem::file_size(ifc, sec);
                            auto src_t2 = std::filesystem::last_write_time(ixx, mec);
                            auto ifc_t2 = std::filesystem::last_write_time(ifc, mec);
                            bool src_regen2 = !mec && !sec && ifc_sz >= 128 && src_t2 > ifc_t2;
                            if (!sec && ifc_sz >= 128 && std::filesystem::exists(obj_dir / (n + ".obj")) && !src_regen2)
                            {
                                fresh = true;
                            }
                        }
                        if (fresh)
                        {
                            if (settings.verbose) std::printf("  [%s] cached\n", n.c_str());
                            continue;
                        }
                        if (settings.verbose) std::printf("  [%s] recompiling (stale or corrupt ifc)\n", n.c_str());
                    }
                    // Not cached (source rewritten, ifc corrupt, or ifc gone):
                    // drop the module's stale .ifc AND .obj up front. The
                    // recompile's outputs must be the only artifacts that can
                    // be linked — a failed recompile must never leave the old
                    // pair behind to be linked as if it were current code.
                    {
                        std::error_code dec;
                        std::filesystem::remove(ifc, dec);
                        std::filesystem::remove(obj_dir / (n + ".obj"), dec);
                    }
                    std::string log_path = abs_obj + "\\" + n + ".log";
                    std::string cl_cmd = "\"" + cl_path + "\" /nologo /c /std:c++latest /EHsc /MD /O2 /DNDEBUG /FS /utf-8 /W4 /bigobj /DWIN32_LEAN_AND_MEAN /DWINRT_LEAN_AND_MEAN /DNOMINMAX";
                    cl_cmd += " /ifcOutput \"" + abs_obj + "\" /ifcSearchDir \"" + abs_obj + "\" /ifcSearchDir \"" + abs_ixx + "\"";
                    cl_cmd += " /Fo\"" + abs_obj + "\\" + n + ".obj\"";
                    cl_cmd += " \"" + ixx.string() + "\"";
                    cl_cmd += " >\"" + log_path + "\" 2>&1";
                    cj_names.push_back(n);
                    cj_cmds.push_back(env_prefix + cl_cmd);
                }

                // Use the existing topological ordering from `ordered`.
                // Since `cj_names` is already a subset of `ordered` in
                // topological order, we can compile safely in chunks:
                // every module in chunk N only depends on modules in
                // chunks 0..N-1 (or earlier in the same chunk).
                // Rebuild the level grouping using the scan's modules:
                std::vector<std::vector<std::size_t>> levels;
                {
                    // Build a map from module name to its index in cj_names
                    std::unordered_map<std::string, std::size_t> cj_idx;
                    for (std::size_t ci = 0; ci < cj_names.size(); ++ci)
                        cj_idx[cj_names[ci]] = ci;
                    // Build a map from module name to its imports. ALL modules
                    // go in (cached ones too): a cached intermediate .ifc is
                    // re-resolved when a consumer loads it, so its imports
                    // still participate in the readiness closure below.
                    std::unordered_map<std::string, std::vector<std::string> const*> imp_map;
                    for (std::size_t mi = 0; mi < modules.size(); ++mi)
                        imp_map[modules[mi].name] = &modules[mi].imports;
                    std::unordered_set<std::string> compiled;
                    // Readiness must consider the TRANSITIVE import closure
                    // through modules being recompiled this run. Direct-import
                    // checking is not enough: a consumer whose direct imports
                    // are all cached (its .ifc already on disk) can still
                    // transitively reach a module being recompiled right now -
                    // e.g. Markup -> root (cached .ifc) -> P1, where P1's .ifc
                    // was deleted up front for its recompile. Loading the cached
                    // root .ifc then re-resolves its export-imports and fails
                    // with C2230 "could not find module". Ordering every
                    // consumer after its transitive deps' recompiles completes
                    // fixes it (cached intermediates only need the final .ifc
                    // present when they are loaded).
                    auto transitive_ready = [&](std::string const& name)
                    {
                        std::unordered_set<std::string> visited;
                        std::vector<std::string> stack{name};
                        while (!stack.empty())
                        {
                            std::string cur = stack.back();
                            stack.pop_back();
                            auto it = imp_map.find(cur);
                            if (it == imp_map.end()) continue;
                            for (auto const& imp : *it->second)
                            {
                                // A module being recompiled this run blocks the
                                // consumer until its new .ifc exists. Cached and
                                // external modules (std, winrt_base) have their
                                // .ifc on disk already - but their imports still
                                // participate: loading a cached root .ifc makes
                                // the compiler re-resolve its export-imports.
                                if (cj_idx.contains(imp) && !compiled.contains(imp)) return false;
                                if (visited.insert(imp).second) stack.push_back(imp);
                            }
                        }
                        return true;
                    };
                    std::size_t remaining = cj_names.size();
                    while (remaining > 0)
                    {
                        std::vector<std::size_t> level;
                        for (std::size_t ci = 0; ci < cj_names.size(); ++ci)
                        {
                            if (compiled.contains(cj_names[ci])) continue;
                            bool ready = transitive_ready(cj_names[ci]);
                            if (ready) level.push_back(ci);
                        }
                        if (level.empty())
                        {
                            // Try direct imports when transitive search is stuck in a cycle
                            for (std::size_t ci = 0; ci < cj_names.size(); ++ci)
                            {
                                if (compiled.contains(cj_names[ci])) continue;
                                bool direct_ready = true;
                                auto it = imp_map.find(cj_names[ci]);
                                if (it != imp_map.end())
                                {
                                    for (auto const& imp : *it->second)
                                    {
                                        if (cj_idx.contains(imp) && !compiled.contains(imp))
                                        {
                                            direct_ready = false;
                                            break;
                                        }
                                    }
                                }
                                if (direct_ready) level.push_back(ci);
                            }
                        }
                        if (level.empty())
                        {
                            // Cycle breaker: pick the uncompiled module with fewest missing deps
                            std::size_t best_ci = std::string::npos;
                            std::size_t min_unsat = std::string::npos;
                            for (std::size_t ci = 0; ci < cj_names.size(); ++ci)
                            {
                                if (compiled.contains(cj_names[ci])) continue;
                                std::size_t unsat = 0;
                                auto it = imp_map.find(cj_names[ci]);
                                if (it != imp_map.end())
                                {
                                    for (auto const& imp : *it->second)
                                        if (cj_idx.contains(imp) && !compiled.contains(imp)) unsat++;
                                }
                                if (unsat < min_unsat)
                                {
                                    min_unsat = unsat;
                                    best_ci = ci;
                                }
                            }
                            if (best_ci != std::string::npos)
                                level.push_back(best_ci);
                        }
                        for (auto idx : level) compiled.insert(cj_names[idx]);
                        levels.push_back(std::move(level));
                        remaining -= levels.back().size();
                    }
                }

                std::size_t num_threads = system_core_count();
                // CPPWINC_JOBS caps compile parallelism: giant module chunks
                // (the 321K-line winrt.Microsoft.UI.Xaml.P1) can each need
                // gigabytes of front-end memory, and N parallel cl.exe on a
                // low-RAM machine OOMs before the first level finishes.
                if (char const* jobs = std::getenv("CPPWINC_JOBS"))
                {
                    unsigned long n = std::strtoul(jobs, nullptr, 10);
                    if (n >= 1 && n < num_threads) num_threads = static_cast<std::size_t>(n);
                }
                std::printf("\n-compiling %zu modules across %zu levels (%zu threads, all cores)\n",
                             cj_names.size(), levels.size(), num_threads);
                auto compile_start = std::chrono::steady_clock::now();
                std::atomic<std::size_t> compiled_ok{0}, compiled_fail{0};
                std::atomic<std::size_t> job_idx{0};

                // A single huge module (the 80MB winrt.Microsoft.UI.Xaml.P1,
                // the 24MB winrt.Windows, ...) needs gigabytes of front-end
                // memory; compiling several of them concurrently makes cl.exe
                // die with no diagnostics (leaving a 1-byte .ifc). Split each
                // level into big modules (compiled one at a time so each has
                // the machine to itself) and small modules (compiled in
                // parallel) so a full clean build stays deterministic.
                auto const ifc_ok = [&](std::string const& mn) {
                    auto ifc = obj_dir / (mn + ".ifc");
                    if (!std::filesystem::exists(ifc)) return false;
                    std::error_code sec;
                    return std::filesystem::file_size(ifc, sec) >= 128;
                };
                // Record a module's compile outcome. A failed compile must
                // leave NO artifacts: remove whatever cl.exe dropped (a 1-byte
                // ifc, a partial obj) so the link phase can never consume stale
                // code and a retry starts clean. (A stale ifc surviving a
                // failed compile used to make ifc_ok report OK and link stale
                // objects.)
                auto record_result = [&](std::string const& mn) {
                    if (ifc_ok(mn)) { compiled_ok++; return; }
                    std::error_code dec;
                    std::filesystem::remove(obj_dir / (mn + ".ifc"), dec);
                    std::filesystem::remove(obj_dir / (mn + ".obj"), dec);
                    compiled_fail++;
                };
                for (auto const& level : levels)
                {
                    std::vector<std::size_t> big_mods, light_mods;
                    for (auto ci : level)
                    {
                        std::error_code sec;
                        auto sz = std::filesystem::file_size(proj_folder / (cj_names[ci] + ".ixx"), sec);
                        if (!sec && sz > 4ULL * 1024 * 1024) big_mods.push_back(ci);
                        else light_mods.push_back(ci);
                    }
                    for (auto ci : big_mods)
                    {
                        if (settings.verbose) std::printf("  [big] %s\n", cj_names[ci].c_str());
                        std::system(cj_cmds[ci].c_str());
                        record_result(cj_names[ci]);
                    }
                    if (light_mods.size() == 1)
                    {
                        std::size_t ci = light_mods[0];
                        std::system(cj_cmds[ci].c_str());
                        record_result(cj_names[ci]);
                    }
                    else if (light_mods.size() > 1)
                    {
                        std::size_t nthreads = num_threads;
                        if (nthreads > light_mods.size()) nthreads = light_mods.size();
                        job_idx = 0;
                        std::vector<std::jthread> workers;
                        workers.reserve(nthreads);
                        for (std::size_t t = 0; t < nthreads; ++t)
                        {
                            workers.emplace_back([&]() {
                                while (true)
                                {
                                    std::size_t idx = job_idx.fetch_add(1);
                                    if (idx >= light_mods.size()) break;
                                    std::size_t ci = light_mods[idx];
                                    std::system(cj_cmds[ci].c_str());
                                    record_result(cj_names[ci]);
                                }
                            });
                        }
                        for (auto& w : workers)
                            if (w.joinable()) w.join();
                    }
                    std::printf("  level done: %zu modules (cumulative: %zu ok / %zu fail)\n",
                                 level.size(), compiled_ok.load(), compiled_fail.load());
                }

                auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - compile_start).count();
                std::printf("-compiled %zu OK, %zu FAILED in %.0fs\n",
                             compiled_ok.load(), compiled_fail.load(), (double)total_elapsed);
                std::size_t ok = compiled_ok.load(), fail = compiled_fail.load();
                if (fail > 0) result = 1;

    // Link phase: app exe (main.ixx present) + driver sys (DriverEntry
    // present) + release DLL (DllMain or a .def file present) — each target
    // links independently so one pass can produce all of them.
    if (result == 0)
    {
        std::vector<std::filesystem::path> app_cpp;
        std::vector<std::filesystem::path> driver_srcs;
        bool has_main = false;
        bool has_dllmain = false;
        std::filesystem::path def_file;
        for (auto const& entry : std::filesystem::directory_iterator(proj_folder))
        {
            if (!entry.is_regular_file()) continue;
            auto fn = entry.path().filename().string();
            for (auto& c : fn) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            auto ext = entry.path().extension().string();
            for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            if (ext == ".cpp")
            {
                app_cpp.push_back(entry.path());
                // driver source? (WDM/KMDF entries — plain text scan)
                std::ifstream df(entry.path());
                std::string dline;
                while (std::getline(df, dline))
                {
                    if (dline.find("DriverEntry") != std::string::npos ||
                        dline.find("WDF_DRIVER_CONFIG") != std::string::npos)
                        driver_srcs.push_back(entry.path());
                    // Shared-library target: a .cpp carrying DllMain marks a
                    // DLL project (same plain-text heuristic as DriverEntry).
                    if (dline.find("DllMain") != std::string::npos)
                        has_dllmain = true;
                }
            }
            else if (fn == "main.ixx")
            {
                has_main = true;
            }
            else if (ext == ".def")
            {
                // Module-definition file also marks a DLL target and feeds /DEF.
                def_file = entry.path();
                has_dllmain = true;
            }
        }

        std::string out_name = proj_folder.lexically_normal().filename().string();
        while (!out_name.empty() && (out_name.back() == '/' || out_name.back() == '\\'))
            out_name.pop_back();

        // Only the final linked binaries land in the -compile/-winui path
        // folder when one was given; build intermediates (objs, logs, ifc)
        // always stay in <project>/build/obj.
        std::filesystem::path final_dir = final_out.empty() ? proj_folder : std::filesystem::path(final_out);
        std::filesystem::create_directories(final_dir);
        // Locate an import library on the same LIB directories the link
        // searches (MSVC toolchain lib + SDK um/ucrt/km x64). Every link
        // flavor passes import libraries by bare name (windowsapp.lib,
        // runtimeobject.lib, onecore.lib, ntoskrnl.lib, the "// Import
        // library:" attributions, ...); SDK revisions rename or retire
        // libraries (combase.lib was folded into windowsapp.lib), so bare
        // names resolve to full paths up front instead of trusting the
        // ambient LIB. Returns "" when no directory carries the library.
        auto locate_lib = [&](std::string const& lib) -> std::string
        {
            std::error_code pec;
            auto probe = [&](std::string const& dir) -> std::string
            {
                if (dir.empty()) return {};
                std::filesystem::path p = std::filesystem::path(dir) / lib;
                return std::filesystem::exists(p, pec) ? p.string() : std::string{};
            };
            std::string found = probe(toolchain_lib);
            if (found.empty() && !sdk_lib.empty())
            {
                found = probe(sdk_lib + "\\um\\x64");
                if (found.empty()) found = probe(sdk_lib + "\\ucrt\\x64");
                if (found.empty()) found = probe(sdk_lib + "\\km\\x64");
            }
            return found;
        };
        // Required WinRT libs: full path when locatable, bare name as a
        // last resort so the linker's own diagnostic still fires, plus an
        // explicit warning so a retired SDK lib is never a silent mystery.
        auto resolve_lib = [&](std::string const& lib) -> std::string
        {
            std::string full = locate_lib(lib);
            if (full.empty())
            {
                std::printf("cppwinc : warning: %s not found in toolchain/SDK lib directories\n", lib.c_str());
                return lib;
            }
            return full;
        };
        auto resolve_winrt_libs = [&]()
        {
            std::vector<std::string> out;
            for (std::string const& n : { "windowsapp.lib", "runtimeobject.lib", "onecore.lib" })
                out.push_back(resolve_lib(n));
            return out;
        };

        auto link_with = [&](std::string const& out_file, std::vector<std::string> const& objs,
                             std::vector<std::string> const& libs, std::string const& flags)
        {
            std::string link_path = (toolchain / "bin" / "Hostx64" / "x64" / "link.exe").string();
            // Hundreds of module objects exceed cmd.exe's 8K command line —
            // pass inputs through a link response file instead.
            std::string rsp_path = (obj_dir / (out_file + ".rsp")).string();
            {
                std::ofstream rsp(rsp_path, std::ios::binary);
                for (auto const& o : objs) rsp << "\"" << o << "\"\n";
                // Emit explicit /LIBPATH: for the C++ runtime and SDK lib dirs.
                // The link must resolve msvcprt.lib / msvcprt[d].lib (MSVC
                // toolset) and ucrt.lib (SDK ucrt) independent of the ambient
                // LIB env var — which is unset outside a VS dev prompt. Relying
                // on it made every harness link fail LNK1104 "cannot open
                // msvcprt.lib" on machines without a launched dev shell. Passing
                // the search paths on the command line makes the driver
                // self-contained.
                {
                    std::error_code pdec;
                    auto push_libpath = [&](std::string const& dir)
                    {
                        if (!dir.empty() && std::filesystem::is_directory(dir, pdec))
                            rsp << "/LIBPATH:\"" << dir << "\"\n";
                    };
                    push_libpath(toolchain_lib); // MSVC toolset: msvcprt.lib, msvcrt.lib
                    if (!sdk_lib.empty())
                    {
                        push_libpath(sdk_lib + "\\ucrt\\x64"); // ucrt.lib (C runtime)
                        push_libpath(sdk_lib + "\\um\\x64");   // um import libs
                    }
                }
                for (auto const& l : libs)
                {
                    // Directory entries are search paths, not input files:
                    // the driver link pushes the WDK km\\x64 folder so
                    // ntoskrnl.lib/hal.lib resolve. Writing it as a quoted
                    // file made link fail LNK1181 on the bare lib names.
                    std::error_code dec;
                    if (std::filesystem::is_directory(l, dec))
                        rsp << "/LIBPATH:\"" << l << "\"\n";
                    else
                    {
                        // Auto-locate bare import-library names (kernel32.lib,
                        // attribution libs, ...) against the toolchain/SDK lib
                        // directories so every link flavor resolves them without
                        // depending on the ambient LIB. Entries that already
                        // carry a path pass through untouched.
                        std::string path = l;
                        if (l.find_first_of("\\/:") == std::string::npos)
                        {
                            std::string full = locate_lib(l);
                            if (!full.empty())
                                path = std::move(full);
                            else if (settings.verbose)
                                std::printf("  [link] %s not located; passing the bare name to the linker\n", l.c_str());
                        }
                        rsp << "\"" << path << "\"\n";
                    }
                }
                rsp << flags << "\n";
            }
            std::string cmd = env_prefix;
            cmd += "\"" + link_path + "\" /NOLOGO \"/OUT:" + (final_dir / out_file).string() + "\"";
            cmd += " @\"" + rsp_path + "\"";
            cmd += " >\"" + (obj_dir / (out_file + ".link.log")).string() + "\" 2>&1";
            std::printf("-linking %s\n", out_file.c_str());
            int rc = std::system(cmd.c_str());
            if (rc != 0 || !std::filesystem::exists(final_dir / out_file))
            {
                std::printf("cppwinc : error linking %s (log: %s)\n", out_file.c_str(),
                            (obj_dir / (out_file + ".link.log")).string().c_str());
                return false;
            }
            std::printf("  produced %s\n", (final_dir / out_file).string().c_str());
            return true;
        };

        std::printf("-link phase reached: result=%d has_main=%d app_cpp=%zu driver=%zu name='%s' folder='%s' exists=%d\n",
                    result, has_main ? 1 : 0, app_cpp.size(), driver_srcs.size(), out_name.c_str(),
                    proj_folder.string().c_str(), std::filesystem::is_directory(proj_folder) ? 1 : 0);
        std::string name = app_name.empty() ? out_name : app_name;
        if (name.empty()) name = "app";

        // Target selection: `-target exe,sys,dll` forces specific links and
        // errors when a forced target has no matching source; without -target
        // every auto-detected target links — one pass can produce the exe,
        // the sys AND the dll together.
        bool const explicit_targets = !settings.targets.empty();
        bool const want_sys = explicit_targets ? settings.targets.contains("sys") : !driver_srcs.empty();
        bool const want_dll = explicit_targets ? settings.targets.contains("dll") : has_dllmain;
        bool const want_exe = !explicit_targets || settings.targets.contains("exe");
        if (explicit_targets && want_sys && driver_srcs.empty())
        {
            std::printf("cppwinc : error -target sys: no driver source (DriverEntry) found in %s\n",
                        proj_folder.string().c_str());
            result = 1;
        }

        // Compile the .cpp app/driver sources now (module objects already
        // exist; .cpp files are outside the .ixx topological pass).
        std::string cl_path = (toolchain / "bin" / "Hostx64" / "x64" / "cl.exe").string();
        for (auto const& src : app_cpp)
        {
            auto obj = obj_dir / (src.filename().string() + ".obj");
            bool fresh = std::filesystem::exists(obj)
                && std::filesystem::last_write_time(obj) > std::filesystem::last_write_time(src);
            if (fresh) continue;
            // Stale (source rewritten or obj partial): drop the old obj so a
            // failed compile can never leave stale code to be linked.
            std::error_code cec;
            std::filesystem::remove(obj, cec);
            std::string cmd = env_prefix + "\"" + cl_path + "\" /nologo /c /std:c++latest /EHsc /MD /O2 /DNDEBUG /FS /utf-8 /W4 /bigobj";
            cmd += " /Fo\"" + obj.string() + "\" \"" + src.string() + "\"";
            cmd += " >\"" + (obj_dir / (src.filename().string() + ".log")).string() + "\" 2>&1";
            std::printf("-compiling %s\n", src.filename().string().c_str());
            if (std::system(cmd.c_str()) != 0 || !std::filesystem::exists(obj))
            {
                std::printf("cppwinc : error compiling %s (log: %s)\n", src.filename().string().c_str(),
                            (obj_dir / (src.filename().string() + ".log")).string().c_str());
                result = 1;
            }
        }
        // A failed .cpp compile must stop the link: never fall through to
        // linking with missing/stale objects.
        if (result != 0) return result;

        // Kernel driver: WDK modules (Windows.Wdk.* / driver .cpp) -> <name>.sys
        if (want_sys && !driver_srcs.empty())
        {
            std::string wdk_inc, wdk_lib;
            {
                std::filesystem::path wdk_root;
                if (const char* pf_x86 = std::getenv("ProgramFiles(x86)"))
                    wdk_root = std::filesystem::path(pf_x86) / "Windows Kits" / "10";
                if (std::filesystem::is_directory(wdk_root))
                {
                    std::error_code wec;
                    std::filesystem::path newest_w;
                    auto inc_root = wdk_root / "Include";
                    if (std::filesystem::is_directory(inc_root, wec))                                for (auto const& e : std::filesystem::directory_iterator(inc_root, wec))
                                {
                                    auto nm = e.path().filename().string();
                                    // Version dirs only — excludes 'wdf'/'wdk'
                                    // folders that would win a string compare.
                                    if (e.is_directory() && !nm.empty() &&
                                        std::isdigit(static_cast<unsigned char>(nm.front()))
                                        && (newest_w.empty() || nm > newest_w.filename().string()))
                                        newest_w = e.path();
                                }
                    if (!newest_w.empty())
                    {
                        wdk_inc = newest_w.string();
                        wdk_lib = (newest_w.parent_path().parent_path() / "Lib" / newest_w.filename().string()).string();
                    }
                }
            }
            std::vector<std::string> drv_objs;
            for (auto const& s : driver_srcs)
                drv_objs.push_back((obj_dir / (s.filename().string() + ".obj")).string());
            for (auto const& n : cj_names)
                if (n.rfind("Windows.Wdk.", 0) == 0 || n.rfind("Windows.Win32.", 0) == 0)
                    drv_objs.push_back((obj_dir / (n + ".obj")).string());
            std::vector<std::string> drv_libs = { "ntoskrnl.lib", "hal.lib", "bufferoverflowfastfailk.lib" };
            if (!wdk_lib.empty()) drv_libs.push_back(wdk_lib + "\\km\\x64");
            std::string drv_flags = "/DRIVER /SUBSYSTEM:NATIVE /ENTRY:GsDriverEntry /MACHINE:X64 /INCREMENTAL:NO /RELEASE /OPT:REF /OPT:ICF";
            if (!link_with(name + ".sys", drv_objs, drv_libs, drv_flags))
                result = 1;
        }
        // Release DLL: DllMain / .def present -> <name>.dll linked in release
        // flavor (objects already come from the /MD /O2 /DNDEBUG compile pass;
        // the link adds /RELEASE /OPT:REF /OPT:ICF like the exe and sys links).
        // Independent of the sys and exe links above.
        if (want_dll)
        {
            std::vector<std::string> dll_objs;
            for (auto const& c : app_cpp)
                dll_objs.push_back((obj_dir / (c.filename().string() + ".obj")).string());
            std::error_code dec;
            for (auto const& o : std::filesystem::directory_iterator(obj_dir, dec))
                if (o.is_regular_file() && o.path().extension() == ".obj"
                    && o.path().filename() != "std.obj" && o.path().filename() != "main.obj")
                    dll_objs.push_back(o.path().string());
            if (std::filesystem::exists(obj_dir / "std.obj"))
                dll_objs.push_back((obj_dir / "std.obj").string());
            std::string dll_flags =
                "/DLL /SUBSYSTEM:WINDOWS /MACHINE:X64 /INCREMENTAL:NO /RELEASE /OPT:REF /OPT:ICF";
            if (!def_file.empty())
                dll_flags += " /DEF:\"" + def_file.string() + "\"";
            // A WinRT component dll needs the same umbrella import libs the
            // exe links use (Ro*/HSTRING live in windowsapp.lib).
            std::vector<std::string> dll_libs;
            if (std::filesystem::exists(obj_dir / "winrt_base.obj"))
                dll_libs = resolve_winrt_libs();
            if (!link_with(name + ".dll", dll_objs, dll_libs, dll_flags))
                result = 1;
        }
        // App exe: main.ixx bootstrap + user .cpp + all module objects ->
        // <name>.exe (the linker pulls only referenced modules, which
        // covers the app's own component module like WinUIGallery.App).
        // Independent of the sys link above: a project folder that carries
        // BOTH a driver source and a main.ixx app must produce both the
        // <name>.sys and the <name>.exe in one pass (the old 'else if'
        // silently dropped the exe whenever a driver was present).
        if (want_exe && has_main)
        {
            std::vector<std::string> app_objs;
            if (std::filesystem::exists(obj_dir / "main.obj"))
                app_objs.push_back((obj_dir / "main.obj").string());
            for (auto const& c : app_cpp)
                app_objs.push_back((obj_dir / (c.filename().string() + ".obj")).string());
            std::error_code aec;
            for (auto const& o : std::filesystem::directory_iterator(obj_dir, aec))
                if (o.is_regular_file() && o.path().extension() == ".obj"
                    && o.path().filename() != "std.obj" && o.path().filename() != "main.obj")
                    app_objs.push_back(o.path().string());
            if (std::filesystem::exists(obj_dir / "std.obj"))
                app_objs.push_back((obj_dir / "std.obj").string());
            std::vector<std::string> app_libs = resolve_winrt_libs();
            std::string bootstrap = locate_wasdk_bootstrap_lib(settings.verbose);
            if (!bootstrap.empty()) app_libs.push_back(bootstrap);
            // The generated bootstrap defines wWinMain (wide entry); the CRT
            // shim that forwards to it is wWinMainCRTStartup. Without an
            // explicit /ENTRY the linker defaults to WinMain and fails with
            // LNK2019 on `WinMain`.
            if (!link_with(name + ".exe", app_objs, app_libs,
                "/SUBSYSTEM:WINDOWS /MACHINE:X64 /INCREMENTAL:NO /ENTRY:wWinMainCRTStartup /RELEASE /OPT:REF /OPT:ICF"))
                result = 1;
        }
        // Console quick-test: a .cpp with plain main() -> <name>.exe linked
        // against the std module object.
        else if (want_exe && !app_cpp.empty())
        {
            bool has_console_main = false;
            {
                // Scan EVERY app source, not just the first: directory
                // iteration order is arbitrary, so a main() in any file
                // (e.g. main.cpp listed after the ex_*.cpp examples) must
                // count for the console-exe link.
                for (auto const& c : app_cpp)
                {
                    std::ifstream mf(c);
                    std::string mline;
                    while (std::getline(mf, mline))
                        if (mline.find("int main(") != std::string::npos) { has_console_main = true; break; }
                    if (has_console_main) break;
                }
            }
            if (has_console_main)
            {
                std::vector<std::string> objs;
                for (auto const& c : app_cpp)
                    objs.push_back((obj_dir / (c.filename().string() + ".obj")).string());
                // every module object in obj_dir feeds the link (linker drops
                // unused ones); covers imports of any projection in the folder
                std::error_code gec;
                for (auto const& o : std::filesystem::directory_iterator(obj_dir, gec))
                    if (o.is_regular_file() && o.path().extension() == ".obj"
                        && o.path().filename() != "std.obj")
                        objs.push_back(o.path().string());
                if (std::filesystem::exists(obj_dir / "std.obj"))
                    objs.push_back((obj_dir / "std.obj").string());
                // WinRT projections pull winrt_base (WindowsCreateString,
                // Ro* activation): link the same import libs the wWinMain
                // path uses, else every WinRT console test dies LNK2019.
                std::vector<std::string> libs;
                if (std::filesystem::exists(obj_dir / "winrt_base.obj"))
                    libs = resolve_winrt_libs();
                // Win32/WDK quick-tests: the extern "C" declarations carry a
                // "// Import library: X.lib" attribution in the generated
                // modules. Collect the distinct names and link them — the
                // console path otherwise passed NO import libs and every
                // Win32 call died LNK2019 (RegOpenKeyExW, CreateWindowExW...).
                {
                    std::set<std::string> seen_libs;
                    std::error_code iec;
                    for (auto const& lf : std::filesystem::directory_iterator(proj_folder, iec))
                    {
                        if (!lf.is_regular_file() || lf.path().extension() != ".ixx") continue;
                        std::ifstream imf(lf.path());
                        std::string iline;
                        while (std::getline(imf, iline))
                        {
                            auto pos = iline.find("// Import library: ");
                            if (pos == std::string::npos) continue;
                            std::string lib = iline.substr(pos + 19);
                            while (!lib.empty() && (lib.back() == '\r' || lib.back() == '\n' || lib.back() == ' ')) lib.pop_back();
                            // The attribution sometimes carries the DLL name
                            // from the import descriptor (kernel32.dll); the
                            // linker wants the import library (kernel32.lib).
                            // The attribution carries whatever spelling the
                            // projection wrote: a DLL name (kernel32.dll), a
                            // driver image (ntoskrnl.exe) or an on-disk lib
                            // name (SDK ships rpcrt4.lib AND RpcRT4.Lib).
                            // The linker wants the import LIBRARY and the
                            // tail test must be case-insensitive — a
                            // case-sensitive ".lib" check silently dropped
                            // every CamelCase .Lib attribution (RpcRT4.Lib…).
                            std::string tail = lib.size() >= 4 ? lib.substr(lib.size() - 4) : lib;
                            for (auto& c : tail) c = (char)std::tolower((unsigned char)c);
                            if (lib.size() > 4 && (tail == ".dll" || tail == ".exe" || tail == ".drv"))
                                lib = lib.substr(0, lib.size() - 4) + ".lib";
                            else if (tail != ".lib") continue;
                            // Same alias as probe_sdk_lib_dir: the WDK CRT
                            // headers attribute to ucrtbase.dll, whose import
                            // library the SDK ships as ucrt.lib. Spelled out
                            // here so the attribution links explicitly instead
                            // of falling back to the ambient default-lib set.
                            if (lib == "ucrtbase.lib") lib = "ucrt.lib";
                            // Static CRT archives are link-line poison for
                            // this driver's /MD model: some C-runtime rows
                            // carry a static attribution (libucrt.lib,
                            // ntstc_libcmt.lib) and passing the archive next
                            // to /MD's ucrt.lib import library redefines the
                            // whole CRT surface (LNK2005 exit/_c_exit/wcslen
                            // ... already defined). The default-lib set the
                            // /MD compile already pulled in resolves every
                            // ucrt-attributed symbol, so skip them.
                            {
                                std::string lower = lib;
                                for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                                static const std::set<std::string> static_crt_libs = {
                                    "libucrt.lib", "libucrtd.lib", "libcmt.lib", "libcmtd.lib",
                                    "libcpmt.lib", "libcpmtd.lib", "ntstc_libcmt.lib", "libcntpr.lib",
                                };
                                if (static_crt_libs.count(lower))
                                {
                                    if (settings.verbose)
                                        std::printf("  [link] skip static CRT lib %s (/MD defaults already provide it)\n", lib.c_str());
                                    continue;
                                }
                            }
                            // SDK revisions retire import libraries (combase.lib
                            // was folded into windowsapp.lib in newer kits): an
                            // attribution naming a library the link cannot open is
                            // a hard LNK1181, so probe the same LIB directories the
                            // link searches and skip what is no longer shipped —
                            // the umbrella import libs already carry those symbols.
                            if (locate_lib(lib).empty())
                            {
                                if (settings.verbose)
                                    std::printf("  [link] skip retired attribution lib %s (not in SDK/toolchain)\n", lib.c_str());
                                continue;
                            }
                            if (seen_libs.insert(lib).second) libs.push_back(lib);
                        }
                    }
                }
                if (!link_with(name + ".exe", objs, libs, "/SUBSYSTEM:CONSOLE /MACHINE:X64 /INCREMENTAL:NO /RELEASE /OPT:REF /OPT:ICF"))
                    result = 1;
            }
            else if (explicit_targets)
            {
                std::printf("cppwinc : error -target exe: no main.ixx and no int main() source found in %s\n",
                            proj_folder.string().c_str());
                result = 1;
            }
        }
        else if (want_exe && explicit_targets)
        {
            std::printf("cppwinc : error -target exe: no main.ixx or .cpp source found in %s\n",
                        proj_folder.string().c_str());
            result = 1;
        }
    }
    return result;
}
