// cppwinc_cli.ixx — CLI parsing, arg_parser, SDK resolution
// Split from cppwinc.ixx for readability.
// This file is #included from cppwinc.ixx, not compiled independently.

// CLI

struct option
{
    std::string_view name;
    std::string_view arg;
    std::string_view desc;
    std::size_t max = 0;
};

struct usage_exception
{
};

constexpr option options[] =
{
    { "input", "<spec>", "Windows metadata to include (ONE comma-separated list: local,<path>,10.0.x). -winui auto-adds WASDK metadata + DLLs", 1 },
    { "output", "<path>", "Location of generated projection", 1 },
    { "component", "[<path>]", "Generate component templates and implementation. Optional <path> = folder receiving the component files (default: the -output folder)", 1 },
    { "name", "<name>", "Specify explicit name for component files", 1 },
    { "verbose", {}, "Show detailed progress information", 0 },
    { "include", "<list>", "Namespace prefixes to include, as ONE comma-separated list or repeated switches (e.g. -include Windows.Win32.Graphics,Windows.Win32.Storage)", 0 },
    { "exclude", "<list>", "Namespace prefixes to exclude, as ONE comma-separated list or repeated switches (e.g. -exclude Windows.Win32.DirectWrite,Windows.Win32.Com) - takes precedence over -include", 0 },
    { "base", {}, "Generate base module unconditionally", 0 },
    { "std", "[<path>]", "Build the std named module into <path> (default: output folder)", 1 },
    { "winui", "[<path>]", "Auto-discover WASDK metadata + DLLs; build Microsoft.UI.Xaml projection if runtime DLLs not found. Optional <path> = folder receiving ONLY the final linked binaries (modules stay in -output)", 1 },
    { "winui_no_compile", "[<path>]", "Like -winui but skip compilation (generate only); same optional <path> outpath support", 1 },
    { "compile", "[<path>]", "Compile modules (release: /MD /O2, parallel across all cores) + link exe / sys / release dll. With -input: compiles the generated projection; optional <path> = folder receiving ONLY the final linked binaries. Alone: quick-test - compiles <path>'s .ixx modules + .cpp sources and links <name>.exe/.sys/.dll (intermediates stay in <path>/build)", 1 },
    { "target", "<list>", "Link targets for -compile (requires -compile): comma-separated exe, sys, dll (default: auto-detect from sources; every matching target links in one pass). Output location: -compile path, else -output, else the project folder", 0 },
    { "help", {}, "Show detailed help with examples", 0 },
    { "?", {}, {}, 0 },
    { "module_include", "<list>", "Only generate modules for the listed namespaces, as ONE comma-separated list or repeated switches (e.g. -module_include Windows.Win32.Graphics,Windows.Win32.Storage)", 0 },
    { "module_exclude", "<list>", "Skip modules for the listed namespaces, as ONE comma-separated list or repeated switches (e.g. -module_exclude Windows.Win32.Com) - takes precedence over -module_include", 0 },
};

option const* find_option(std::string_view name)
{
    for (auto const& opt : options)
    {
        if (opt.name == name)
        {
            return &opt;
        }
    }
    return nullptr;
}

std::string to_utf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    int const size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// Split the raw command line the way Windows users actually type it.
// The CRT's CommandLineToArgvW treats a backslash before a closing quote
// as an escape, so a quoted path ending in '\' ("C:\dir\", "..\")
// loses its closing quote and swallows the rest of the command line.
// Here a quoted token whose closing quote is preceded by an odd number
// of backslashes keeps them (trailing path separator) and ends the
// token — the standard Windows convention for quoted paths.
std::vector<std::string> split_command_line()
{
    std::wstring line = GetCommandLineW();
    std::vector<std::string> result;
    std::wstring current;
    bool in_quotes = false;
    std::size_t i = 0;
    while (i < line.size())
    {
        wchar_t const c = line[i];
        if (c == L'\\')
        {
            // a run of backslashes possibly followed by a quote
            std::size_t run = 0;
            while (i + run < line.size() && line[i + run] == L'\\')
            {
                ++run;
            }
            if (i + run < line.size() && line[i + run] == L'"')
            {
                bool closing = (i + run + 1 >= line.size() || line[i + run + 1] == L' ' || line[i + run + 1] == L'\t');
                if (in_quotes && closing && (run % 2 == 1))
                {
                    // quoted path ending in a backslash ("C:\dir\"):
                    // keep the backslashes as path separators and close
                    // the quoted token instead of swallowing the rest of
                    // the command line
                    current.append(run, L'\\');
                    in_quotes = false;
                }
                else
                {
                    // standard rule: 2n backslashes before a quote yield n
                    // backslashes and a quote that toggles; 2n+1 yield n
                    // backslashes and a literal (escaped) quote
                    current.append(run / 2, L'\\');
                    if (run % 2 == 1)
                    {
                        current.push_back(L'"');
                    }
                    else
                    {
                        in_quotes = !in_quotes;
                    }
                }
                i += run + 1;
            }
            else
            {
                current.append(run, L'\\');
                i += run;
            }
        }
        else if (c == L'"')
        {
            in_quotes = !in_quotes;
            ++i;
        }
        else if (c == L' ' || c == L'\t')
        {
            if (in_quotes)
            {
                current.push_back(c);
            }
            else if (!current.empty())
            {
                result.push_back(to_utf8(current));
                current.clear();
            }
            ++i;
        }
        else
        {
            current.push_back(c);
            ++i;
        }
    }
    if (!current.empty())
    {
        result.push_back(to_utf8(current));
    }
    return result;
}

struct arg_parser
{
    std::map<std::string, std::vector<std::string>, std::less<>> args;

    // Resolve a relative switch path ("..\\out", "foo.winmd") against the
    // current directory so every later open/save works regardless of where
    // the user launched the exe from. Absolute paths pass through unchanged.
    static std::string normalize_path(std::string const& value)
    {
        if (value.size() < 2) return value;
        if (value[1] == ':') return value;                       // drive-absolute M:\...
        if (value[0] == '/' || value[0] == '\\') return value;   // rooted
        std::error_code ec;
        auto abs = std::filesystem::absolute(value, ec);
        if (ec) return value;
        auto canon = std::filesystem::weakly_canonical(abs, ec);
        if (ec) return abs.string();
        return canon.string();
    }

    // A single -input list element is only a path when it looks like one:
    // contains a separator or names something that exists relative to cwd.
    // Keywords ("local", "wasdk") and SDK versions ("10.0.26100.0") don't.
    static bool looks_like_path(std::string const& value)
    {
        if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos) return true;
        return std::filesystem::exists(std::filesystem::path(value));
    }

    // Resolve relative paths in every path-taking switch after parsing so
    // all consumers (driver, compiler, linker) see absolute paths.
    void normalize_path_args()
    {
        for (std::string const& key : { "output", "component", "std", "compile", "winui", "winui_no_compile" })
        {
            auto it = args.find(key);
            if (it == args.end()) continue;
            for (auto& v : it->second) v = normalize_path(v);
        }
        auto it = args.find("input");
        if (it == args.end()) return;
        for (auto& list : it->second)
        {
            std::string out;
            std::size_t pos = 0;
            while (pos <= list.size())
            {
                auto comma = list.find(',', pos);
                std::string elem = list.substr(pos, (comma == std::string::npos ? list.size() : comma) - pos);
                if (!elem.empty() && looks_like_path(elem)) elem = normalize_path(elem);
                out += elem;
                if (comma == std::string::npos) break;
                out += ',';
                pos = comma + 1;
            }
            list = out;
        }
    }

    explicit arg_parser(std::vector<std::string> const& raw)
    {
        std::size_t i = 0;
        while (i < raw.size())
        {
            std::string const& arg = raw[i];
            if (arg.size() < 2 || arg[0] != '-')
            {
                throw usage_exception{};
            }
            std::string name = arg.substr(1);
            option const* opt = find_option(name);
            if (!opt)
            {
                throw std::runtime_error("unknown option: " + name);
            }
            std::vector<std::string> values;
            ++i;
            while (i < raw.size() && !(raw[i].size() >= 2 && raw[i][0] == '-' && find_option(raw[i].substr(1))))
            {
                values.push_back(raw[i]);
                ++i;
            }
            if (opt->max != 0 && values.size() > opt->max)
            {
                throw std::runtime_error("too many values for option: " + name
                    + "\n  -input takes ONE comma-separated list (e.g. -input local,wasdk,MyApp.winmd)");
            }
            auto& dst = args[name];
            if (opt->max != 0 && !dst.empty())
            {
                throw std::runtime_error("duplicate option: -" + name
                    + (name == "input"
                        ? " (pass all inputs in ONE comma-separated list, e.g. -input local,MyApp.winmd)"
                        : ""));
            }
            dst.insert(dst.end(), values.begin(), values.end());
        }
        normalize_path_args();
        // -compile / -winui / -winui_no_compile are mutually exclusive build
        // modes: -winui* generate (+compile) the WASDK Xaml projection,
        // -compile consumes a projection (or quick-tests a folder). Combining
        // them would run two conflicting build pipelines over one output dir.
        if (args.contains("compile") && (args.contains("winui") || args.contains("winui_no_compile")))
        {
            throw std::runtime_error("-compile cannot be combined with -winui or -winui_no_compile"
                "\n  -winui / -winui_no_compile generate AND compile the WASDK projection;"
                "\n  -compile alone compiles an existing projection (with -input) or quick-tests a folder.");
        }
        if (args.contains("winui") && args.contains("winui_no_compile"))
        {
            throw std::runtime_error("-winui and -winui_no_compile are mutually exclusive"
                "\n  (-winui_no_compile already skips compilation).");
        }
    }

    bool exists(std::string_view name) const
    {
        return args.contains(std::string(name));
    }

    std::vector<std::string> const& values(std::string_view name) const
    {
        static std::vector<std::string> empty;
        auto it = args.find(std::string(name));
        return it == args.end() ? empty : it->second;
    }

    std::string value(std::string_view name, std::string_view fallback = {}) const
    {
        auto const& v = values(name);
        return v.empty() ? std::string(fallback) : v.front();
    }
};

// SDK / input resolution

// Reads a REG_SZ value with the wide registry APIs and a dynamically
// sized buffer: registry values are paths, and a fixed-size buffer would
// truncate installs on machines with deep paths (the long-path
// restriction is lifted for the whole process by the longPathAware
// manifest, so the buffer must not re-impose it).
std::wstring registry_string(std::wstring const& key_path, std::wstring const& value_name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return {};
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key, value_name.c_str(), nullptr, &type, nullptr, &size);
    if (result != ERROR_SUCCESS || type != REG_SZ)
    {
        RegCloseKey(key);
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(key, value_name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
    {
        return {};
    }
    if (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return value;
}

std::wstring registry_string_either(std::wstring const& key_path, std::wstring const& value_name)
{
    std::wstring value = registry_string(key_path, value_name);
    if (!value.empty())
    {
        return value;
    }
    // some installs register the Windows SDK only in the 32-bit view
    // If key_path already starts with "SOFTWARE\", replace it with
    // "SOFTWARE\WOW6432Node\" instead of doubling the prefix.
    std::wstring wow64_key;
    if (key_path.size() >= 8 && key_path.compare(0, 8, L"SOFTWARE\\") == 0)
    {
        wow64_key = L"SOFTWARE\\WOW6432Node\\" + key_path.substr(8);
    }
    else
    {
        wow64_key = L"SOFTWARE\\WOW6432Node\\" + key_path;
    }
    return registry_string(wow64_key, value_name);
}

std::vector<std::filesystem::path> resolve_sdk()
{
    // Collect candidate SDK root directories
    std::vector<std::filesystem::path> candidate_roots;
    
    // 1. Try registry
    std::wstring root = registry_string_either(L"SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows\\v10.0", L"InstallationFolder");
    if (!root.empty())
        candidate_roots.push_back(std::filesystem::path(root));
    
    // 2. Well-known paths as fallback (when the registry is missing).
    // Roots come from the environment — %ProgramFiles(x86)% / %ProgramFiles%
    // / %SystemDrive% — never a hardcoded drive letter, so this resolves on
    // any machine regardless of where Windows is installed.
    if (const wchar_t* pf_x86 = _wgetenv(L"ProgramFiles(x86)"))
        candidate_roots.push_back(std::filesystem::path(pf_x86) / L"Windows Kits" / L"10");
    if (const wchar_t* pf_env = _wgetenv(L"ProgramFiles"))
        candidate_roots.push_back(std::filesystem::path(pf_env) / L"Windows Kits" / L"10");
    if (candidate_roots.empty())
    {
        if (const wchar_t* sys_drive = _wgetenv(L"SystemDrive"))
        {
            candidate_roots.push_back(std::filesystem::path(sys_drive) / L"\\Program Files (x86)\\Windows Kits\\10");
            candidate_roots.push_back(std::filesystem::path(sys_drive) / L"\\Program Files\\Windows Kits\\10");
        }
    }
    
    std::wstring version = registry_string_either(L"SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows\\v10.0", L"ProductVersion");
    
    std::vector<std::filesystem::path> result;
    auto try_dir = [&](std::filesystem::path const& dir) {
        if (!std::filesystem::is_directory(dir)) return;
        for (auto const& entry : std::filesystem::directory_iterator(dir))
        {
            if (entry.path().extension() == ".winmd")
                result.push_back(entry.path());
        }
    };
    
    for (auto const& sdk_root : candidate_roots)
    {
        if (!std::filesystem::is_directory(sdk_root)) continue;
        // Try multiple metadata locations:
        // 1. root/version/Windows Metadata/ (older SDKs)
        if (!version.empty()) try_dir(sdk_root / version / L"Windows Metadata");
        // 2. root/UnionMetadata/version/ (newer SDKs like 10.0.26100+)
        if (!version.empty()) try_dir(sdk_root / L"UnionMetadata" / version);
        // 3. root/UnionMetadata/ (flat)
        try_dir(sdk_root / L"UnionMetadata");
        // 4. Scan subdirectories of UnionMetadata for version folders
        auto union_meta = sdk_root / L"UnionMetadata";
        if (std::filesystem::is_directory(union_meta))
        {
            for (auto const& entry : std::filesystem::directory_iterator(union_meta))
            {
                if (entry.is_directory())
                    try_dir(entry.path());
            }
        }
        if (!result.empty()) break;
    }
    
    if (result.empty())
    {
        throw std::runtime_error("cannot locate Windows SDK metadata");
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Discover WASDK winmds from NuGet package cache.
// Searches ~/.nuget/packages/microsoft.windowsappsdk.winui/<latest>/metadata/
// and  ~/.nuget/packages/microsoft.windowsappsdk.foundation/<latest>/metadata/
std::vector<std::filesystem::path> resolve_wasdk()
{
    std::vector<std::filesystem::path> result;
    // NuGet global packages folder (shared resolver: NUGET_PACKAGES first,
    // then %USERPROFILE%/.nuget/packages)
    std::filesystem::path nuget_root = nuget_packages_root();
    if (nuget_root.empty()) return result;
    // WASDK WinUI winmds (Microsoft.UI.Xaml, Microsoft.UI.Text)
    std::filesystem::path winui_pkg = nuget_root / L"microsoft.windowsappsdk.winui";
    if (std::filesystem::is_directory(winui_pkg))
    {
        // Find latest version
        std::filesystem::path latest_ver;
        for (auto const& entry : std::filesystem::directory_iterator(winui_pkg))
        {
            if (entry.is_directory())
            {
                auto const& p = entry.path().filename().string();
                if (!p.empty() && std::isdigit(static_cast<unsigned char>(p.front())))
                {
                    if (latest_ver.empty() || p > latest_ver.filename().string())
                    {
                        latest_ver = entry.path();
                    }
                }
            }
        }
        if (!latest_ver.empty())
        {
            auto meta = latest_ver / L"metadata";
            if (std::filesystem::is_directory(meta))
            {
                for (auto const& entry : std::filesystem::directory_iterator(meta))
                {
                    if (entry.path().extension() == ".winmd")
                        result.push_back(entry.path());
                }
            }
        }
    }
    // WASDK Foundation winmds (Microsoft.Windows.* from foundation pkg)
    std::filesystem::path found_pkg = nuget_root / L"microsoft.windowsappsdk.foundation";
    if (std::filesystem::is_directory(found_pkg))
    {
        std::filesystem::path latest_ver;
        for (auto const& entry : std::filesystem::directory_iterator(found_pkg))
        {
            if (entry.is_directory())
            {
                auto const& p = entry.path().filename().string();
                if (!p.empty() && std::isdigit(static_cast<unsigned char>(p.front())))
                {
                    if (latest_ver.empty() || p > latest_ver.filename().string())
                    {
                        latest_ver = entry.path();
                    }
                }
            }
        }
        if (!latest_ver.empty())
        {
            auto meta = latest_ver / L"metadata";
            if (std::filesystem::is_directory(meta))
            {
                for (auto const& entry : std::filesystem::directory_iterator(meta))
                {
                    if (entry.path().extension() == ".winmd")
                        result.push_back(entry.path());
                }
            }
        }
    }
    // WASDK base package — provides Microsoft.UI.winmd, Microsoft.Foundation.winmd,
    // Microsoft.Graphics.winmd (base type definitions used by all Microsoft.UI.* projections)
    // Scan ALL versions since newer versions (2.x) may not have these base winmds
    std::filesystem::path base_pkg = nuget_root / L"microsoft.windowsappsdk";
    if (std::filesystem::is_directory(base_pkg))
    {
        std::set<std::string> found_stems;
        for (auto const& ver_entry : std::filesystem::directory_iterator(base_pkg))
        {
            if (!ver_entry.is_directory()) continue;
            auto const& ver = ver_entry.path().filename().string();
            if (ver.empty() || !std::isdigit(static_cast<unsigned char>(ver.front()))) continue;
            // Scan lib subdirs for winmds (1.5.x layout: lib/uap10.0.18362/*.winmd)
            std::error_code ec;
            auto lib_dir = ver_entry.path() / L"lib";
            if (!std::filesystem::is_directory(lib_dir, ec)) continue;
            for (auto const& lib_entry : std::filesystem::recursive_directory_iterator(lib_dir, ec))
            {
                if (lib_entry.path().extension() == L".winmd")
                {
                    auto stem = lib_entry.path().stem().string();
                    // Only take base type winmds we haven't found yet
                    if ((stem == "Microsoft.UI" || stem == "Microsoft.Foundation" ||
                         stem == "Microsoft.Graphics") && !found_stems.contains(stem))
                    {
                        found_stems.insert(stem);
                        result.push_back(lib_entry.path());
                    }
                }
            }
        }
    }
    // Deduplicate by filename (prefer winui/foundation pkg versions)
    std::set<std::filesystem::path> seen;
    std::vector<std::filesystem::path> deduped;
    for (auto const& p : result)
    {
        auto name = p.filename();
        if (seen.insert(name).second)
            deduped.push_back(p);
    }
    std::sort(deduped.begin(), deduped.end());
    return deduped;
}

std::vector<std::filesystem::path> resolve_local()
{
    // The Windows directory comes from the OS itself (GetWindowsDirectoryW),
    // never a hardcoded drive letter, so this works on any machine and any
    // drive. GetWindowsDirectoryW returns the required size (including the
    // NUL) when the buffer is too small, so grow until it fits — a fixed
    // MAX_PATH buffer would be overrun on machines with deep paths.
    std::wstring windows_dir;
    std::size_t capacity = MAX_PATH;
    for (;;)
    {
        windows_dir.resize(capacity);
        UINT length = ::GetWindowsDirectoryW(windows_dir.data(), static_cast<UINT>(windows_dir.size()));
        if (length == 0)
        {
            windows_dir.clear();
            break;
        }
        if (length < windows_dir.size())
        {
            windows_dir.resize(length);
            break;
        }
        capacity = static_cast<std::size_t>(length) + 1;
    }
    if (windows_dir.empty())
    {
        throw std::runtime_error("cannot locate the Windows directory");
    }
    std::filesystem::path root(windows_dir);
    // 32-bit processes see %WinDir%\System32 redirected to SysWOW64, which
    // does not hold the OS WinMetadata; Sysnative is the alias that reaches
    // the real System32 from a 32-bit process. A 64-bit process sees
    // System32 directly (Sysnative does not exist for it), so probe both
    // and deduplicate.
    std::vector<std::filesystem::path> dirs;
    std::filesystem::path sysnative = root / "Sysnative" / "WinMetadata";
    std::filesystem::path system32 = root / "System32" / "WinMetadata";
    if (std::filesystem::is_directory(sysnative))
    {
        dirs.push_back(sysnative);
    }
    if (std::filesystem::is_directory(system32))
    {
        dirs.push_back(system32);
    }
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    for (auto const& dir : dirs)
    {
        for (auto const& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (entry.path().extension() == ".winmd")
            {
                result.push_back(entry.path());
            }
        }
    }
    if (result.empty())
    {
        throw std::runtime_error("cannot locate local WinRT metadata (" + (root / "System32" / "WinMetadata").string() + ")");
    }
    std::sort(result.begin(), result.end());
    // Sysnative and System32 can resolve to the same physical folder on
    // some systems; drop duplicates so the projection is not double-loaded
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::filesystem::path> resolve_one_spec(std::string_view spec)
{
    if (spec == "local")
    {
        return resolve_local();
    }
    if (spec.size() >= 3 && spec[0] == '1' && spec[1] == '0' && spec[2] == '.')
    {
        std::wstring root = registry_string_either(L"SOFTWARE\\Microsoft\\Microsoft SDKs\\Windows\\v10.0", L"InstallationFolder");
        std::filesystem::path metadata = std::filesystem::path(root) / std::wstring(spec.begin(), spec.end()) / L"Windows Metadata";
        std::vector<std::filesystem::path> result;
        if (std::filesystem::is_directory(metadata))
        {
            for (auto const& entry : std::filesystem::directory_iterator(metadata))
            {
                if (entry.path().extension() == ".winmd")
                {
                    result.push_back(entry.path());
                }
            }
        }
        if (result.empty())
        {
            throw std::runtime_error("cannot locate Windows SDK metadata for: " + std::string(spec));
        }
        std::sort(result.begin(), result.end());
        return result;
    }
    std::filesystem::path path(spec);
    std::vector<std::filesystem::path> result;
    if (std::filesystem::is_directory(path))
    {
        for (auto const& entry : std::filesystem::recursive_directory_iterator(path))
        {
            if (entry.path().extension() == ".winmd")
            {
                result.push_back(entry.path());
            }
        }
    }
    else if (std::filesystem::is_regular_file(path))
    {
        result.push_back(path);
    }
    else
    {
        throw std::runtime_error("cannot find: " + std::string(spec));
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Accept comma-separated specs: -input A.winmd,B.winmd,local
std::vector<std::filesystem::path> resolve_spec(std::string_view spec)
{
    std::vector<std::filesystem::path> result;
    std::size_t start = 0;
    while (start <= spec.size())
    {
        auto comma = spec.find(',', start);
        auto len = (comma == std::string_view::npos) ? spec.size() - start : comma - start;
        auto sub = spec.substr(start, len);
        while (!sub.empty() && sub.front() == ' ') sub.remove_prefix(1);
        while (!sub.empty() && sub.back() == ' ') sub.remove_suffix(1);
        if (!sub.empty())
        {
            auto paths = resolve_one_spec(sub);
            result.insert(result.end(), paths.begin(), paths.end());
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return result;
}

