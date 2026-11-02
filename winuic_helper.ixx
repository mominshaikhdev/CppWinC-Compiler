// winuic_helper.ixx — module winuic_helper.
// Helper functions for code generation: string escaping, property name
// resolution, value-to-C++ expression mapping, CSS color conversion.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <sstream>

export module winuic_helper;

export namespace winuic
{

// Identifier spelling of a metadata namespace / module name.
// App namespaces follow the shared hyphen naming default ("WinUI Gallery"
// -> "WinUI-Gallery"), which is a legal ECMA-335 metadata string but NOT a
// C++ identifier: every character that cannot appear in an identifier maps
// to '_' (dots are kept - they separate segments in module names and are
// rewritten to "::" for C++ namespace qualification). STRING contexts
// (class full names, activation keys, the winmd itself) keep the raw
// metadata spelling; call this only where the token becomes real C++.
inline std::string ident_ns(std::string_view ns)
{
    std::string out;
    out.reserve(ns.size());
    for (char c : ns)
    {
        bool const keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9') || c == '_' || c == '.';
        out += keep ? c : '_';
    }
    return out;
}

// Escape a raw value for use inside L"..." wide string literal.
// Replaces backslashes first, then quotes, then newlines/tabs/cr.
inline std::string escape_wstr(std::string const& s)
{
    std::string r;
    r.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': r += "\\\\"; break;
        case '"':  r += "\\\""; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:   r += c; break;
        }
    }
    return r;
}

// Wrap a raw string value into a L"..." wide string literal, with escaping.
inline std::string wstr_literal(std::string const& raw)
{
    // Strip existing L"..." wrapping to prevent double L-strings
    std::string s = raw;
    if (s.size() > 2 && s[0] == 'L' && s[1] == '"')
        s = s.substr(2, s.size() - 3);
    return "L\"" + escape_wstr(s) + "\"";
}

// Remove one level of L"..." / "..." string-literal wrapping if present.
inline std::string strip_wstr(std::string s)
{
    if (s.size() > 2 && s[0] == 'L' && s[1] == '"')
        s = s.substr(2, s.size() - 3);
    else if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    return s;
}

// Normalize escaped quotes and string-literal wrapping in JSON payloads.
// Resource fragments may arrive as L"..." literals; downstream XAML loaders
// need the XML payload, not the C++ literal syntax.
inline std::string normalize_payload_value(std::string const& raw)
{
    std::string v = raw;
    // unescape \" -> " first (JSON string-escape style)
    std::string unescaped;
    unescaped.reserve(v.size());
    for (std::size_t i = 0; i < v.size(); ++i)
    {
        if (v[i] == '\\' && i + 1 < v.size() && (v[i + 1] == '"' || v[i + 1] == '\\'))
        {
            unescaped += v[++i];
        }
        else
        {
            unescaped += v[i];
        }
    }
    v = unescaped;
    // Strip one pair of surrounding quotes, including a wide-string prefix.
    if (v.size() >= 3 && v[0] == 'L' && v[1] == '"' && v.back() == '"')
    {
        v = v.substr(2, v.size() - 3);
    }
    else if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
    {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

// true when a METADATA type name is a primitive numeric type (winmd element
// types projecting to arithmetic C++). Type knowledge only - never control or
// property names - so a new control ships in metadata without a rebuild.
inline bool is_numeric_type_name(std::string_view tn)
{
    return tn == "double" || tn == "float" || tn == "int8_t" || tn == "uint8_t"
        || tn == "int16_t" || tn == "uint16_t" || tn == "int32_t" || tn == "uint32_t"
        || tn == "int64_t" || tn == "uint64_t" || tn == "char16_t";
}

// true when text is a plain numeric expression (digits, '.', sign, e/E, and
// optionally a wrapping sign+parentheses like "-(50)" or "(100)").
inline bool is_number_text(std::string raw)
{
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) raw.erase(0, 1);
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
    if (raw.empty()) return false;
    // Unwrap whole-expression parentheses: "-(50)" -> "-50", "(100)" -> "100"
    if ((raw.front() == '-' || raw.front() == '+') && raw.size() >= 3
        && raw[1] == '(' && raw.back() == ')')
    {
        raw = raw.substr(0, 1) + raw.substr(2, raw.size() - 3);
    }
    else if (raw.front() == '(' && raw.back() == ')' && raw.size() >= 2)
        raw = raw.substr(1, raw.size() - 2);
    if (raw.empty()) return false;
    bool any_digit = false;
    for (unsigned char c : raw)
    {
        if (std::isdigit(c)) { any_digit = true; continue; }
        if (c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') continue;
        return false;
    }
    return any_digit;
}

// Metadata-driven typed-value resolution. Replaces the legacy
// control/property enum tables and the bare-string -> enum name list: the
// property's TYPE comes from the loaded winmds and nowhere else, so a new
// control, enum or property value ships in metadata only - no cppwinc
// rebuild.
//
//   prop_kinds  lowercased type name -> {lowercased property -> metadata
//               type name}, flattened over base classes and interfaces
//               (driver-built: g_type_prop_kinds)
//   enums       enum type name -> (namespace, member names)
//               (driver-built: g_enum_info)
//
// The single rule, keyed on the metadata type:
//   enum-typed property + a value that is a member of that enum
//       -> winrt::<ns>::<Type>::<Member>
// Bare L"...", quoted and already-qualified values all work, and a value
// an earlier pass qualified with the WRONG enum gets re-qualified, because
// membership is checked against the metadata enum (ScrollView props are
// Scrolling* enums while ScrollViewer props are plain Scroll* ones,
// Tooltip.Placement is PlacementMode, ...) - no control-name branches.
// Any other value (unknown property, non-member, markup/structured
// literal) is returned unchanged.
inline std::string resolve_typed_value(
    std::string const& control, std::string_view pk, std::string val,
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> const* prop_kinds,
    std::unordered_map<std::string, std::pair<std::string, std::set<std::string>>> const* enums)
{
    if (!prop_kinds || !enums || val.empty()) return val;

    // lowercased control leaf + property name (g_type_prop_kinds keys)
    std::string lo_c = control;
    if (auto const csc = lo_c.rfind("::"); csc != std::string::npos) lo_c = lo_c.substr(csc + 2);
    for (auto& c : lo_c) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lo_p(pk);
    for (auto& c : lo_p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lo_c.empty() || lo_p.empty()) return val;

    // The property's metadata type name ("" when the winmds do not know it).
    std::string ptype;
    if (auto const tit = prop_kinds->find(lo_c); tit != prop_kinds->end())
        if (auto const pit = tit->second.find(lo_p); pit != tit->second.end())
            ptype = pit->second;
    if (ptype.empty()) return val;

    // Candidate member: last :: segment, unwrapped from L"..." / "...",
    // whitespace-trimmed.
    std::string mem = val;
    if (auto const mq = mem.rfind("::"); mq != std::string::npos) mem = mem.substr(mq + 2);
    mem = strip_wstr(mem);
    while (!mem.empty() && mem.front() == ' ') mem.erase(0, 1);
    while (!mem.empty() && mem.back() == ' ') mem.pop_back();
    if (mem.empty()) return val;

    // Nullable numeric props: metadata says IReference<double> (generic
    // args come from the winmd - axis Minimum/Maximum are the gallery's
    // users), so box the number the way the projected setter wants it.
    if (ptype == "IReference<double>")
    {
        auto const is_num = [](std::string const& s) {
            if (s.empty()) return false;
            bool dot = false;
            std::size_t i = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
            if (i >= s.size()) return false;
            for (; i < s.size(); ++i)
            {
                if (s[i] == '.') { if (dot) return false; dot = true; }
                else if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            }
            return true;
        };
        if (is_num(mem))
        {
            std::string n = mem;
            if (n.find('.') == std::string::npos) n += ".0";
            return "winrt::Windows::Foundation::IReference<double>(winrt::box_value(" + n + "))";
        }
        return val;
    }

    // Only an enum the metadata declares for this property can qualify it.
    auto const eit = enums->find(ptype);
    if (eit == enums->end()) return val;
    bool member = false;
    for (auto const& m : eit->second.second)
        if (m == mem) { member = true; break; }
    if (!member) return val;

    std::string ns_cpp;
    for (char c : eit->second.first) ns_cpp += (c == '.' ? "::" : std::string(1, c));
    return "winrt::" + ns_cpp + "::" + ptype + "::" + mem;
}

// true when the control's IsChecked property is plain bool (menu flyout items)
// vs IReference<bool> (CheckBox/RadioButton/ToggleButton/AppBarToggleButton)
inline bool is_plain_bool_ischecked(std::string const& control)
{
    std::string lo_c(control);
    // named_elements stores fully-qualified C++ types
    // (winrt::Microsoft::UI::Xaml::Controls::CheckBox); the plain-bool check is
    // about the control kind, so only the last :: segment decides.
    auto csc = lo_c.rfind("::");
    if (csc != std::string::npos && csc + 2 < lo_c.size())
        lo_c = lo_c.substr(csc + 2);
    for (auto& c : lo_c) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lo_c.find("menuflyoutitem") != std::string::npos) return true;
    if (lo_c.find("togglebutton") != std::string::npos) return false;
    if (lo_c == "checkbox" || lo_c == "radiobutton") return false;
    return true;
}

// fixup_view_code: deterministic text post-pass applied to each generated view
// module.  The emitter has many parallel value-resolution paths; this pass
// canonicalizes the emitted C++ so setters receive exactly what the (flat,
// no-inheritance) projection accepts.  All rules are shape-based and only fire
// on lines that look like generated `var.Prop(...)` setter calls.
//
// type_events / prop_kinds: metadata-derived control knowledge populated by
// the driver from the loaded winmds (lowercased type name -> event set, and
// lowercased type name -> {lowercased property -> metadata type name}).
// Rules that would otherwise hardcode per-control names consult these maps so
// new controls/events ship in metadata only - never a cppwinc rebuild.
inline std::string fixup_view_code(std::string code,
    std::unordered_map<std::string, std::set<std::string>> const* type_events = nullptr,
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> const* prop_kinds = nullptr,
    std::unordered_map<std::string, std::string> const* type_namespaces = nullptr,
    std::unordered_map<std::string, std::pair<std::string, std::set<std::string>>> const* enums = nullptr)
{
    auto replace_all = [](std::string& s, std::string const& from, std::string const& to)
    {
        if (from == to || from.empty()) return; // no-op guard: same-text replace would loop forever
        while (true)
        {
            auto q = s.find(from);
            if (q == std::string::npos) break;
            s.replace(q, from.size(), to);
        }
    };
    auto trim = [](std::string s)
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        return s;
    };
    auto lower = [](std::string s)
    {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    // Split into lines.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : code) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur.push_back(c); }
        if (!cur.empty()) lines.push_back(cur);
    }

    // Map declared element variables to their projected type name.
    std::map<std::string, std::string> vtype;
    for (auto const& ln : lines)
    {
        auto p = ln.find("auto ");
        if (p == std::string::npos) continue;
        std::string s = ln.substr(p + 5);
        size_t i = 0;
        std::string id;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) id.push_back(s[i++]);
        if (id.empty() || id == "const") continue;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size() || s[i] != '=') continue;
        ++i;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        std::string ty;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == ':'))
            ty.push_back(s[i++]);
        // Only "auto Name = Type();" element declarations.  Type may be a bare
        // class name or fully qualified (winrt::...::Type) - keep the last segment.
        if (!ty.empty())
        {
            auto sc = ty.rfind("::");
            std::string last = (sc == std::string::npos) ? ty : ty.substr(sc + 2);
            if (!last.empty() && std::isupper(static_cast<unsigned char>(last[0])))
                vtype[id] = last;
        }
    }

    // State-var declarations: every winuic state var projects as a
    // winrt::hstring placeholder (no code-behind collections exist). Track
    // their names for the R43 enumerable-source rule.
    std::map<std::string, bool> hstring_vars;
    for (auto const& lnh : lines)
    {
        size_t php = lnh.find_first_not_of(" \t");
        if (php == std::string::npos) continue;
        if (lnh.compare(php, 15, "winrt::hstring ") != 0) continue;
        php += 15;
        std::string hid;
        while (php < lnh.size() && (std::isalnum(static_cast<unsigned char>(lnh[php])) || lnh[php] == '_')) hid.push_back(lnh[php++]);
        if (hid.empty()) continue;
        while (php < lnh.size() && (lnh[php] == ' ' || lnh[php] == '\t')) ++php;
        if (php < lnh.size() && (lnh[php] == ';' || lnh[php] == '=')) hstring_vars[hid] = true;
    }

    // Determine the receiver var + property name of a generated setter line.
    // Returns the var name (declared element) or empty when the line is not a
    // simple `var.Prop(...)` setter call on a declared element.
    auto receiver_prop = [&](std::string const& ln, std::string& prop_out) -> std::string
    {
        std::string t = trim(ln);
        if (t.empty() || t.rfind("//", 0) == 0) return {};
        size_t i = 0;
        std::string id;
        while (i < t.size() && (std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_')) id.push_back(t[i++]);
        if (i >= t.size() || t[i] != '.') return {};
        if (!vtype.count(id)) return {};
        ++i;
        std::string prop;
        while (i < t.size() && (std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_')) prop.push_back(t[i++]);
        if (i >= t.size() || t[i] != '(') return {};
        prop_out = prop;
        return id;
    };
    // Extract the balanced argument text of the setter call starting at '('.  Returns false on failure.
    auto extract_arg = [](std::string const& ln, std::string& arg_out) -> bool
    {
        auto o = ln.find('(');
        if (o == std::string::npos) return false;
        int depth = 0;
        bool in_str = false;
        for (size_t i = o; i < ln.size(); ++i)
        {
            char c = ln[i];
            if (in_str)
            {
                if (c == '\\') { ++i; continue; }
                if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '(') ++depth;
            else if (c == ')') { --depth; if (depth == 0) { arg_out = ln.substr(o + 1, i - o - 1); return true; } }
        }
        return false;
    };
    auto strip_wstr2 = [&](std::string v)
    {
        v = trim(v);
        if (v.size() >= 3 && v[0] == 'L' && v[1] == '"' && v.back() == '"') v = v.substr(2, v.size() - 3);
        else if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    };
    auto is_plain_numeric = [&](std::string v)
    {
        v = strip_wstr2(v);
        bool ok = !v.empty();
        bool any = false;
        for (unsigned char c : v)
        {
            if (std::isdigit(c)) { any = true; continue; }
            if (c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') continue;
            ok = false; break;
        }
        return ok && any;
    };
    auto last_member = [&](std::string v)
    {
        auto q = v.rfind("::");
        if (q != std::string::npos) v = v.substr(q + 2);
        while (!v.empty() && (v.back() == ' ' || v.back() == ')')) v.pop_back();
        return v;
    };

    for (auto& ln : lines)
    {
        std::string prop;
        std::string recv = receiver_prop(ln, prop);
        // No blanket continue: receiver-less helper lines (inspectable_append(...))
        // still need the line-shape rules (container fixes) below.
        std::string lo_recv = lower(recv);
        std::string rtype = recv.empty() ? std::string() : vtype.at(recv);
        std::string lo_type = lower(rtype);
        std::string lo_prop = lower(prop);
        std::string arg;
        bool has_arg = extract_arg(ln, arg);
        std::string argT = trim(arg);
        // Metadata type name of this receiver.property ("" when unknown).
        // Rules consult the winmd through this instead of hardcoded
        // property-name lists: new controls arrive as metadata only.
        std::string ptype;
        if (prop_kinds && !lo_type.empty() && !lo_prop.empty())
        {
            auto pk0 = prop_kinds->find(lo_type);
            if (pk0 != prop_kinds->end())
            {
                auto pp0 = pk0->second.find(lo_prop);
                if (pp0 != pk0->second.end()) ptype = pp0->second;
            }
        }

        bool comment = false;
        std::string out_line = ln;
        if (has_arg && type_namespaces && enums
            && argT.find("theme_lookup_th(") != std::string::npos
            && !enums->contains(ptype))
        {
            if (auto const type_it = type_namespaces->find(ptype); type_it != type_namespaces->end())
            {
                std::string ns_cpp;
                for (char c : type_it->second)
                    ns_cpp += c == '.' ? "::" : std::string(1, c);
                std::string wrapped = "winrt::" + ns_cpp + "::" + ptype
                    + "::from_abi([](winrt::IInspectable const& __resource) { "
                    + "if (__resource.get_abi()) winrt::IUnknown::addref_raw(__resource.get_abi()); "
                    + "return __resource.get_abi(); }(" + argT + "))";
                if (auto const arg_pos = out_line.find(arg); arg_pos != std::string::npos)
                {
                    out_line.replace(arg_pos, arg.size(), wrapped);
                    ln = out_line;
                    arg = wrapped;
                    argT = wrapped;
                }
            }
        }

        // R44 (shape-based): a setter/binding line carrying a colon-separated
        // literal outside any string (Duration(0:0:0.2)) can never be valid
        // C++ - raw XAML time syntax leaked through. String contents are
        // exempt (ms-appx:/...), as are ternaries. No names involved, so
        // unknown/stub receivers are covered too.
        {
            std::size_t sp = 0;
            while (sp < ln.size() && (ln[sp] == ' ' || ln[sp] == '\t')) ++sp;
            bool already_commented = ln.compare(sp, 2, "//") == 0;
            bool has_setter = false;
            for (std::size_t i = 0; i + 1 < ln.size(); ++i)
            {
                if (ln[i] == '.' && (std::isalnum(static_cast<unsigned char>(ln[i + 1])) || ln[i + 1] == '_'))
                {
                    std::size_t j = i + 1;
                    while (j < ln.size() && (std::isalnum(static_cast<unsigned char>(ln[j])) || ln[j] == '_')) ++j;
                    if (j < ln.size() && ln[j] == '(') { has_setter = true; break; }
                }
            }
            if (!already_commented && has_setter)
            {
                bool in_str = false, in_chr = false, seen_q = false, bad = false;
                for (std::size_t i = 0; i < ln.size(); ++i)
                {
                    char c = ln[i];
                    if (in_str)  { if (c == '\\') { ++i; continue; } if (c == '"')  in_str = false;  continue; }
                    if (in_chr)  { if (c == '\\') { ++i; continue; } if (c == '\'') in_chr = false;  continue; }
                    if (c == '"') { in_str = true;  continue; }
                    if (c == '\'') { in_chr = true;  continue; }
                    if (c == '?') { seen_q = true; continue; }
                    if (c == ':')
                    {
                        bool dbl = (i + 1 < ln.size() && ln[i + 1] == ':') || (i > 0 && ln[i - 1] == ':');
                        if (!dbl && !seen_q) { bad = true; break; }
                    }
                }
                if (bad) comment = true;
            }
        }

        // R1: ScrollView (new control, exact type) uses Scrolling* enums.
        //      ScrollViewer keeps plain Scroll*/ZoomMode enums - never match it.
        if (lo_type == "scrollview")
        {
            bool sv_prop = lo_prop.find("scrollbarvisibility") != std::string::npos
                || lo_prop.find("scrollmode") != std::string::npos
                || lo_prop == "zoommode" || lo_prop == "contentorientation";
            if (sv_prop)
            {
                replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::", "winrt::Microsoft::UI::Xaml::Controls::ScrollingScrollBarVisibility::");
                replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::", "winrt::Microsoft::UI::Xaml::Controls::ScrollingScrollMode::");
                replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::ZoomMode::", "winrt::Microsoft::UI::Xaml::Controls::ScrollingZoomMode::");
                replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::ScrollViewContentOrientation::", "winrt::Microsoft::UI::Xaml::Controls::ScrollingContentOrientation::");
                replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::ScrollContentOrientation::", "winrt::Microsoft::UI::Xaml::Controls::ScrollingContentOrientation::");
                // leftover L"member" on ScrollView enum props -> member enum
                if (argT.size() >= 3 && argT[0] == 'L' && argT[1] == '"' && argT.back() == '"')
                {
                    std::string mem = strip_wstr2(argT);
                    std::string ns;
                    if (lo_prop.find("scrollbarvisibility") != std::string::npos) ns = "winrt::Microsoft::UI::Xaml::Controls::ScrollingScrollBarVisibility::";
                    else if (lo_prop.find("scrollmode") != std::string::npos) ns = "winrt::Microsoft::UI::Xaml::Controls::ScrollingScrollMode::";
                    else if (lo_prop == "zoommode") ns = "winrt::Microsoft::UI::Xaml::Controls::ScrollingZoomMode::";
                    else ns = "winrt::Microsoft::UI::Xaml::Controls::ScrollingContentOrientation::";
                    out_line = out_line.substr(0, out_line.rfind(argT)) + ns + mem + out_line.substr(out_line.rfind(argT) + argT.size());
                }
                replace_all(out_line, "ScrollingScrollBarVisibility::Disabled", "ScrollingScrollBarVisibility::Hidden");
            }
        }
        // R5: ScrollBar.IndicatorMode is Primitives::ScrollingIndicatorMode.
        else if (lo_type.find("scrollbar") != std::string::npos && lo_prop.find("indicatormode") != std::string::npos)
        {
            replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBarIndicatorMode::", "winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollingIndicatorMode::");
            replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::ScrollingIndicatorMode::", "winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollingIndicatorMode::");
        }
        // R2: SplitView.DisplayMode takes SplitViewDisplayMode enum.
        else if (lo_type == "splitview" && lo_prop == "displaymode" && has_arg)
        {
            std::string mem = strip_wstr2(argT);
            if (mem.empty()) mem = last_member(argT);
            if (!mem.empty() && mem.find(' ') == std::string::npos)
                out_line = out_line.substr(0, out_line.rfind(argT)) + "winrt::Microsoft::UI::Xaml::Controls::SplitViewDisplayMode::" + mem + out_line.substr(out_line.rfind(argT) + argT.size());
        }
        // R3: ToolTip.Placement takes Primitives::PlacementMode.
        else if (lo_type == "tooltip" && lo_prop == "placement")
        {
            replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::", "winrt::Microsoft::UI::Xaml::Controls::Primitives::PlacementMode::");
        }
        // R4: TeachingTip.PreferredPlacement takes TeachingTipPlacementMode.
        else if (lo_type == "teachingtip" && lo_prop == "preferredplacement")
        {
            replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::", "winrt::Microsoft::UI::Xaml::Controls::TeachingTipPlacementMode::");
            // leftover literal (PreferredPlacement(L"Auto")) -> enum member
            if (argT.size() >= 3 && argT[0] == 'L' && argT.back() == '"')
            {
                std::string mem = strip_wstr2(argT);
                if (!mem.empty() && mem.find(' ') == std::string::npos)
                {
                    std::string to = "winrt::Microsoft::UI::Xaml::Controls::TeachingTipPlacementMode::" + mem;
                    out_line = out_line.substr(0, out_line.rfind(argT)) + to + out_line.substr(out_line.rfind(argT) + argT.size());
                }
            }
        }
        // R42: TeachingTip.PlacementMargin(x) is a Thickness.
        else if (lo_type == "teachingtip" && lo_prop == "placementmargin" && has_arg)
        {
            std::string a = trim(argT);
            bool bare = !a.empty();
            for (unsigned char c : a)
                if (!(std::isdigit(c) || c == '-' || c == '.')) { bare = false; break; }
            if (bare)
            {
                std::string rep = "winrt::Microsoft::UI::Xaml::Thickness{" + a + "}";
                out_line = out_line.substr(0, out_line.rfind(argT)) + rep + out_line.substr(out_line.rfind(argT) + argT.size());
            }
        }
        // R6: numeric DPs reject string literals (Width(L"Auto"), Value(L"NaN")).
        //     Gated on the winmd property type (primitive numeric), not a name
        //     list; state-variable expressions are left untouched.
        if (has_arg && is_numeric_type_name(ptype) && argT.size() >= 2
            && (argT[0] == 'L' || argT[0] == '\"') && argT.back() == '\"' && !is_plain_numeric(argT))
            comment = true;
        // R7: uniform numeric Thickness props (Padding(12), Margin(0),
        //      BorderThickness(0,1,0,0) — commas included).
        if (!comment && has_arg && (lo_prop == "padding" || lo_prop == "margin" || lo_prop == "borderthickness"))
        {
            std::string a = trim(argT);
            bool bare = !a.empty();
            for (unsigned char c : a) if (!(std::isdigit(c) || c == '-' || c == '.' || c == ',')) { bare = false; break; }
            if (bare)
            {
                std::string rep = "winrt::Microsoft::UI::Xaml::Thickness{" + a + "}";
                out_line = out_line.substr(0, out_line.rfind(argT)) + rep + out_line.substr(out_line.rfind(argT) + argT.size());
            }
        }
        // R8: NineGrid(a,b,c,d) is one Thickness.
        if (!comment && lo_prop == "ninegrid" && has_arg && argT.find(',') != std::string::npos)
        {
            std::string a = trim(argT);
            if (a.find('{') == std::string::npos)
            {
                std::string rep = "winrt::Microsoft::UI::Xaml::Thickness{" + a + "}";
                out_line = out_line.substr(0, out_line.rfind(argT)) + rep + out_line.substr(out_line.rfind(argT) + argT.size());
            }
        }
        // R13: SelectionHighlightColor needs SolidColorBrush, not Brush.
        if (lo_prop == "selectionhighlightcolor")
            replace_all(out_line, "make_solid_brush(", "solid_color_brush(");
        // R9: FontWeight takes the Windows.UI.Text.FontWeight struct; map the
        //      named literal to its numeric weight.
        if (!comment && lo_prop == "fontweight" && has_arg
            && argT.size() >= 3 && argT[0] == 'L' && argT.back() == '"')
        {
            std::string nm = strip_wstr2(argT);
            struct { char const* n; int w; } fw[] = {
                {"Thin",100},{"ExtraLight",200},{"Light",300},{"Normal",400},
                {"Regular",400},{"Medium",500},{"SemiBold",600},{"Semibold",600},
                {"Bold",700},{"ExtraBold",800},{"Black",900},{"Heavy",900},{nullptr,0}
            };
            std::string rep;
            for (int fi = 0; fw[fi].n; ++fi)
                if (nm == fw[fi].n) { rep = "winrt::Windows::UI::Text::FontWeight{" + std::to_string(fw[fi].w) + "}"; break; }
            if (!rep.empty())
                out_line = out_line.substr(0, out_line.rfind(argT)) + rep + out_line.substr(out_line.rfind(argT) + argT.size());
        }
        // R10: SelectedValue is IInspectable -> box enum member or literal.
        if (!comment && lo_prop == "selectedvalue" && has_arg)
        {
            if (argT.find("box_value") == std::string::npos)
            {
                std::string mem = strip_wstr2(argT);
                if (argT.find("::") != std::string::npos) mem = last_member(argT);
                if (!mem.empty() && mem.find(' ') == std::string::npos && mem.find('(') == std::string::npos)
                {
                    std::string boxed = "winrt::box_value(L\"" + mem + "\")";
                    out_line = out_line.substr(0, out_line.rfind(argT)) + boxed + out_line.substr(out_line.rfind(argT) + argT.size());
                }
            }
        }
        // R11: WebView2.Source takes Uri (module stub Uri derives IInspectable);
        //      MediaPlayerElement.Source is IInspectable-only -> box_value.
        if (!comment && lo_prop == "source" && has_arg
            && argT.size() >= 3 && argT[0] == 'L' && argT[1] == '"' && argT.back() == '"')
        {
            if (lo_type == "webview2")
            {
                std::string url = strip_wstr2(argT);
                // strip trailing () artifacts from parsed attribute values
                while (url.size() >= 2 && url.back() == ')' && url[url.size() - 2] == '(')
                    url.pop_back(), url.pop_back();
                std::string uri = "winrt::Windows::Foundation::Uri(L\"" + url + "\")";
                out_line = out_line.substr(0, out_line.rfind(argT)) + uri + out_line.substr(out_line.rfind(argT) + argT.size());
            }
            else if (lo_type.find("mediaplayer") != std::string::npos)
            {
                std::string boxed = "winrt::box_value(" + argT + ")";
                out_line = out_line.substr(0, out_line.rfind(argT)) + boxed + out_line.substr(out_line.rfind(argT) + argT.size());
            }
        }
        // R12: CollectionViewSource.ItemsPath takes PropertyPath (unprojected ctor) -> skip.
        if (!comment && lo_type.find("collectionviewsource") != std::string::npos && lo_prop == "itemspath")
            comment = true;
        // R14: x:Bind garbage chains referencing non-element/undeclared heads.
        if (!comment && has_arg)
        {
            std::string a = trim(argT);
            std::string head;
            size_t k = 0;
            while (k < a.size() && (std::isalnum(static_cast<unsigned char>(a[k])) || a[k] == '_')) head.push_back(a[k++]);
            if (!head.empty() && k < a.size() && a[k] == '.' && !vtype.count(head))
            {
                // head is not a generated element: any .Member() read of a state
                // field / x:Bind element we can't materialize gets commented.
                static const char* bad_members[] = {
                    "IsChecked", "IsOpen", "Value", "IsOn", "SelectedValue",
                    "Character", "IsIconVisible", "IsClosable", "Play", "Stop",
                    "IsTextTrimmed", "SelectedItem", "CurrentItem", nullptr
                };
                std::string rest = a.substr(k + 1);
                for (int bi = 0; bad_members[bi]; ++bi)
                {
                    if (rest.rfind(bad_members[bi], 0) == 0)
                    {
                        comment = true;
                        break;
                    }
                }
                // member-access chains on state fields (calendarIdentifier.SelectedValue())
                if (!comment && rest.rfind("SelectedValue", 0) == 0) comment = true;
            }
            // GetFavoriteGlyph(FavoriteButton.IsChecked)() style: any .IsChecked used
            // inside an argument (not the setter name) is a stale x:Bind reference.
            if (!comment && a.find(".IsChecked") != std::string::npos && a.find("winrt::") == std::string::npos)
                comment = true;
        }
        // R36: event registration with a bare non-element identifier or an
        //      L"handler" literal (TextBox.GotFocus(findBoxHighlightMatches),
        //      ListView.DragOver(L"Source_DragOver")).
        if (!comment && has_arg)
        {
            // Event-shaped argument on a property the METADATA declares as an
            // event of the receiver's type (driver-flattened over base +
            // interfaces). The raw XAML string can never be wired here - the
            // .onX / events[] path emits the real add_<E> registration - so the
            // literal / bare-identifier form is commented. Membership comes from
            // the winmd-derived map passed by the caller: new controls and new
            // events ship in metadata only, never a cppwinc rebuild.
            bool is_evt = false;
            if (type_events && !rtype.empty())
            {
                auto teit = type_events->find(lo_type);
                if (teit != type_events->end())
                    is_evt = teit->second.count(lo_prop) != 0;
            }
            if (is_evt)
            {
                std::string a2 = trim(argT);
                bool literal = a2.size() >= 2 && a2[0] == 'L' && a2.back() == '"';
                bool bare_ident = !a2.empty();
                for (unsigned char c : a2)
                    if (!(std::isalnum(c) || c == '_')) { bare_ident = false; break; }
                if ((bare_ident && !vtype.count(a2)) || literal) comment = true;
            }
        }
        // R34: Visibility with a string-literal argument (x:Bind residue).
        // The setter takes the Visibility enum — never an hstring — so both
        // the call-looking form (L"subtitleText.IsTextTrimmed()") and the
        // bare-identifier form (L"isExperimental", an unresolved binding
        // root) are unmaterializable; comment them out like R14/R15c.
        if (!comment && (lo_prop == "visibility" || lo_prop == "visible") && has_arg
            && argT.size() >= 3 && argT.back() == '"'
            && (argT[0] == '"' || (argT[0] == 'L' && argT[1] == '"')))
            comment = true;
        // R15: ContextFlyout(var) needs FlyoutBase::from_abi wrapper.
        if (!comment && lo_prop == "contextflyout" && has_arg)
        {
            std::string a = trim(argT);
            std::string head;
            size_t k = 0;
            while (k < a.size() && (std::isalnum(static_cast<unsigned char>(a[k])) || a[k] == '_')) head.push_back(a[k++]);
            if (k == a.size() && vtype.count(head) && a.find("from_abi") == std::string::npos)
            {
                std::string wrap = "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutBase::wrap_borrowed(" + head + ".m_unknown.get())";
                out_line = out_line.substr(0, out_line.rfind(argT)) + wrap + out_line.substr(out_line.rfind(argT) + argT.size());
            }
            else if (k == a.size() && !vtype.count(head))
                comment = true; // R15c: not an element we own (state field) -> comment
        }
        // R17: Padding on receivers without the property.
        if (!comment && lo_prop == "padding"
            && (lo_type.find("fonticon") != std::string::npos || lo_type == "popup" || lo_type == "itemsrepeater"))
            comment = true;
        // R17b: Content on receivers without Content.
        if (!comment && lo_prop == "content" && (lo_type == "slider" || lo_type == "infobadge"))
            comment = true;
        // R17f: Prefix on InfoBar (x:Bind conditional residue).
        if (!comment && lo_type == "infobar" && lo_prop == "prefix")
            comment = true;
        // R17c: layout children appended through .Items() on ItemsView/ItemsRepeater -
        //      the projection exposes no Items collection there.
        if (!comment
            && (lo_type == "itemsview" || lo_type == "itemsrepeater")
            && (ln.find(".Items().Append(") != std::string::npos || lo_prop == "items"))
            comment = true;
        // R17h: VAR.Items()/VAR.SwipeItems() on controls that expose no such
        //      collection in this projection (receiver may be inside a helper call,
        //      so scan the declared element map rather than the line head).
        if (!comment && ln.find(".Items()") != std::string::npos)
        {
            for (auto const& [vid, vty] : vtype)
            {
                std::string lvty = lower(vty);
                if ((lvty == "itemsview" || lvty == "itemsrepeater")
                    && ln.find(vid + ".Items()") != std::string::npos)
                {
                    comment = true;
                    break;
                }
            }
        }
        if (!comment && ln.find(".SwipeItems()") != std::string::npos)
        {
            for (auto const& [vid, vty] : vtype)
            {
                if (lower(vty) == "swipecontrol" && ln.find(vid + ".SwipeItems()") != std::string::npos)
                {
                    comment = true;
                    break;
                }
            }
        }
        // R17i: Children().Append on receivers with no Children collection.
        // The inspectable_append(RECV.Children(), item) emission shape hides the
        // receiver from receiver_prop(), so parse that form here as well —
        // otherwise these lines skip this rule and fail with C2039 ('Children'
        // is not a member) on exactly the types listed below.
        std::string r17i_prop = lo_prop;
        std::string r17i_type = lo_type;
        if (r17i_prop != "children")
        {
            std::string_view hap = "inspectable_append(";
            auto hq = ln.find(hap);
            if (hq != std::string::npos)
            {
                size_t h = hq + hap.size();
                while (h < ln.size() && (ln[h] == ' ' || ln[h] == '\t')) ++h;
                std::string hid;
                while (h < ln.size() && (std::isalnum(static_cast<unsigned char>(ln[h])) || ln[h] == '_')) hid.push_back(ln[h++]);
                if (h < ln.size() && ln[h] == '.')
                {
                    ++h;
                    std::string hp;
                    while (h < ln.size() && (std::isalnum(static_cast<unsigned char>(ln[h])) || ln[h] == '_')) hp.push_back(ln[h++]);
                    // exactly `RECV.Prop()` as the sole argument expression head
                    if (h + 1 < ln.size() && ln[h] == '(' && ln[h + 1] == ')' && vtype.count(hid))
                    {
                        r17i_prop = hp;
                        for (auto& c : r17i_prop) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        r17i_type = lower(vtype.at(hid));
                    }
                }
            }
        }
        if (!comment && r17i_prop == "children"
            && (r17i_type == "animatedvisualplayer" || r17i_type.find("xamluicommand") != std::string::npos
                || r17i_type == "itemsrepeaterscrollhost" || r17i_type == "geometrygroup"
                || r17i_type == "path" || r17i_type == "ellipsegeometry"))
            comment = true;
        // Data-driven composition guard: attaching a child through a content/
        // child property the receiver's metadata does not declare cannot
        // compile (C2039). prop_kinds is the flattened winmd + control-catalog
        // property table, so this covers platform, third-party and app
        // receivers with no hardcoded type lists — the object stays declared,
        // only the attach line is commented, exactly like R17i does for
        // Children().
        if (!comment && prop_kinds
            && (r17i_prop == "content" || r17i_prop == "child"))
        {
            auto tit = prop_kinds->find(r17i_type);
            if (tit != prop_kinds->end() && !tit->second.empty()
                && !tit->second.count(r17i_prop))
                comment = true;
        }
        // R17j: SwipeControl has no SwipeItems collection.
        if (!comment && lo_type == "swipecontrol" && lo_prop == "swipeitems")
            comment = true;
        // R17d: Points on Shape types (stubs lack Points).
        if (!comment && lo_prop == "points" && (lo_type == "polyline" || lo_type == "polygon"))
            comment = true;
        // R37: geometry point props with numeric pairs; Rect with literal; no
        //      Point/Rect types are projected.
        if (!comment && (lo_prop == "startpoint" || lo_prop == "endpoint" || lo_prop == "center")
            && argT.find(',') != std::string::npos && argT.find('{') == std::string::npos
            && argT.find('(') == std::string::npos)
            comment = true;
        if (!comment && (lo_type == "rectanglegeometry")
            && lo_prop == "rect" && argT.size() >= 3 && argT[0] == 'L' && argT.back() == '"')
            comment = true;
        // GeometryGroup.Children().Append(...) - GeometryCollection has no Append.
        if (!comment && ln.find("GeometryGroup_") != std::string::npos
            && ln.find(".Children().Append(") != std::string::npos)
            comment = true;
        // R39: non-UIElement stubs appended to element collections.
        // IInspectable-stub types (converters, animation markers, toolkit
        // presenters, template selectors) have no UIElement base, so any
        // Append of one into a UIElementCollection cannot compile. These are
        // property-element constructs from the winui source (x:Bind-era
        // Implicit.Animations, CVS resources, toolkit parts) - the object is
        // still declared and its state-var assignment kept, only the append
        // goes (XAML would resolve them via the resource system, not the tree).
        if (!comment && has_arg)
        {
            static const char* non_uie_types[] = {
                "OffsetAnimation", "OpacityAnimation",
                "MenuItemTemplateSelector", "AnimatedSettingsVisualSource",
                "LottieLogo1", "ValidatedPasswordBox",
                "CollectionVisibilityConverter", "EmptyCollectionToObjectConverter",
                "StringVisibilityConverter", "VariedImageSizeLayout", nullptr
            };
            std::string a39 = trim(arg);
            // bare identifier argument only (the emitted form for these is
            // exactly `X.Children().Append(VarName);`)
            bool bare39 = !a39.empty();
            for (unsigned char c39 : a39)
                if (!(std::isalnum(c39) || c39 == '_')) { bare39 = false; break; }
            if (bare39 && !vtype.count(a39))
            {
                // argument is not a generated element var: dead x:Bind-era
                // reference (e.g. ContactsCVS resource) - comment it.
                if (ln.find(".Children().Append(") != std::string::npos
                    || ln.find(".Items().Append(") != std::string::npos
                    || ln.find("inspectable_append(") != std::string::npos)
                    comment = true;
            }
            else if (bare39 && vtype.count(a39))
            {
                std::string aty = lower(vtype.at(a39));
                for (int i39 = 0; non_uie_types[i39]; ++i39)
                {
                    if (aty == lower(non_uie_types[i39]))
                    {
                        if (ln.find(".Children().Append(") != std::string::npos
                            || ln.find(".Items().Append(") != std::string::npos
                            || ln.find("inspectable_append(") != std::string::npos)
                            comment = true;
                        break;
                    }
                }
            }
            // track which element vars got dead appends: their later uses in
            // the same line already handled; mark var for no other rewrite.
        }
        // R38: PlacementRect(4 args) - no Rect type projected.
        if (!comment && lo_prop == "placementrect")
            comment = true;
        // R33: RichTextBlock/Overflow OverflowContentTarget(var) - from_abi needs
        //      a real instance; the container vars are empty proxies.
        if (!comment && lo_prop == "overflowcontenttarget")
            comment = true;
        // R30: CalendarView/ItemsView/TreeView SelectionMode uses its own enum.
        if (!comment && lo_prop == "selectionmode"
            && (lo_type == "calendarview" || lo_type == "itemsview" || lo_type == "treeview"))
        {
            std::string to_enum = "winrt::Microsoft::UI::Xaml::Controls::";
            to_enum += rtype; // type name == enum name prefix (CalendarView... etc)
            to_enum += "SelectionMode::";
            replace_all(out_line, "winrt::Microsoft::UI::Xaml::Controls::ListViewSelectionMode::", to_enum);
        }
        // R31: Slider.SnapsTo(L"...") -> Primitives::SliderSnapsTo enum.
        if (!comment && lo_prop == "snapsto" && lo_type == "slider" && has_arg
            && argT.size() >= 3 && argT[0] == 'L' && argT.back() == '"')
        {
            std::string mem = strip_wstr2(argT);
            if (!mem.empty() && mem.find(' ') == std::string::npos)
            {
                std::string to = "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::" + mem;
                out_line = out_line.substr(0, out_line.rfind(argT)) + to + out_line.substr(out_line.rfind(argT) + argT.size());
            }
        }
        // R41 (metadata-driven): a numeric literal passed to a property whose
        // metadata type is GridLength (RowDefinition, TableView columns, any
        // future control) is wrapped as GridLength(px, Pixel). The numeric
        // attribute emission would otherwise produce a bare double the setter
        // rejects (C2665). Double-typed numeric props (MinWidth/Height/...)
        // are left untouched - membership comes from the winmd, not a list.
        auto gridlength_wrap = [&](std::string const& value, std::string const& unit)
        {
            auto wq = out_line.rfind(argT);
            if (wq == std::string::npos) return;
            out_line = out_line.substr(0, wq)
                + "winrt::Microsoft::UI::Xaml::GridLength(" + value
                + ", winrt::Microsoft::UI::Xaml::GridUnitType::" + unit + ")"
                + out_line.substr(wq + argT.size());
        };
        if (!comment && has_arg && is_plain_numeric(argT) && ptype == "GridLength")
            gridlength_wrap(argT, "Pixel");
        // R41b (metadata-driven): the emitter also passes the XAML string
        // forms to GridLength props ("*", "Auto", "120"). Convert them to
        // the real struct; a string that cannot be a GridLength is commented.
        if (!comment && has_arg && ptype == "GridLength" && argT.size() >= 2
            && (argT[0] == 'L' || argT[0] == '\"') && argT.back() == '\"')
        {
            std::string mem = trim(strip_wstr2(argT));
            if (mem == "Auto" || mem == "auto")
            {
                gridlength_wrap("0", "Auto");
            }
            else if (!mem.empty() && mem.back() == '*')
            {
                std::string num = trim(mem.substr(0, mem.size() - 1));
                if (num.empty()) num = "1";
                if (is_plain_numeric(num)) gridlength_wrap(num, "Star");
                else comment = true;
            }
            else if (is_number_text(mem))
            {
                gridlength_wrap(mem, "Pixel");
            }
            else
            {
                comment = true;
            }
        }
        // R42: winrt::Windows::Foundation::Uri requires an ABSOLUTE URI. The
        // winui converter reduces {ThemeResource Key} Source attributes to a
        // bare resource name (Uri(L"StoreIcon")) which throws E_INVALIDARG at
        // runtime -> XAML FailFast during OnLaunched. Rooted paths ("/Assets/…")
        // are valid app-relative XAML paths -> normalize to ms-appx:. A bare
        // key cannot be resolved here -> comment the setter out.
        if (!comment)
        {
            auto uq = ln.find("Uri(L\"");
            std::string ltrim_probe = ln;
            // skip already-commented lines (trim leading whitespace, expect //)
            size_t pr = 0;
            while (pr < ltrim_probe.size() && (ltrim_probe[pr] == ' ' || ltrim_probe[pr] == '\t')) ++pr;
            bool already_commented = ltrim_probe.compare(pr, 2, "//") == 0;
            if (uq != std::string::npos && !already_commented)
            {
                size_t q = uq + 6; // past `Uri(L"`
                std::string lit;
                bool closed = false;
                while (q < ln.size())
                {
                    char c = ln[q];
                    if (c == '\\' && q + 1 < ln.size()) { lit += c; lit += ln[q + 1]; q += 2; continue; }
                    if (c == '"') { closed = true; break; }
                    lit += c; ++q;
                }
                if (closed && lit.find(':') == std::string::npos)
                {
                    if (!lit.empty() && lit[0] == '/')
                    {
                        // rooted app path -> absolute ms-appx: URI
                        auto wq = out_line.find("Uri(L\"");
                        if (wq != std::string::npos)
                        {
                            size_t ws = wq + 6;
                            size_t we = out_line.find('"', ws);
                            if (we != std::string::npos)
                                out_line = out_line.substr(0, ws) + "ms-appx:" + lit + out_line.substr(we);
                        }
                    }
                    else
                    {
                        comment = true; // bare resource key / empty -> unresolvable
                    }
                }
            }
        }
        // R43: enumerable-source setters must receive a collection. State vars
        // are all winrt::hstring placeholders and {x:Bind ...} residuals arrive
        // boxed (box_value) - ItemsSourceView's ctor rejects both with
        // E_INVALIDARG at set time (crash while building the page). Keep the
        // call shape (binding lambdas re-run the same line); pass nullptr,
        // i.e. an empty list - truthful: the code-behind collections never
        // exist in this projection.
        if (!comment)
        {
            size_t pr3 = 0;
            while (pr3 < ln.size() && (ln[pr3] == ' ' || ln[pr3] == '\t')) ++pr3;
            bool cm3 = ln.compare(pr3, 2, "//") == 0;
            static const char* src_props[] = { "ItemsSource", "MenuItemsSource", "TabItemsSource", nullptr };
            for (int sp = 0; !comment && src_props[sp]; ++sp)
            {
                if (cm3) break;
                std::string needle = std::string(".") + src_props[sp] + "(";
                auto sq = ln.find(needle);
                if (sq == std::string::npos) continue;
                size_t a0 = sq + needle.size();
                int depth = 1;
                bool in_str = false;
                size_t e0 = a0;
                for (; e0 < ln.size(); ++e0)
                {
                    char c = ln[e0];
                    if (in_str)
                    {
                        if (c == '\\') { ++e0; continue; }
                        if (c == '"') in_str = false;
                        continue;
                    }
                    if (c == '"') { in_str = true; continue; }
                    if (c == '(') ++depth;
                    else if (c == ')') { --depth; if (depth == 0) break; }
                }
                if (e0 >= ln.size()) continue;
                std::string sarg = trim(ln.substr(a0, e0 - a0));
                bool bad = sarg.rfind("winrt::box_value", 0) == 0;
                if (!bad && !sarg.empty())
                {
                    bool ident = true;
                    for (unsigned char c : sarg)
                        if (!(std::isalnum(c) || c == '_')) { ident = false; break; }
                    if (ident && hstring_vars.count(sarg)) bad = true;
                }
                if (bad)
                {
                    auto o3 = out_line.find(needle);
                    if (o3 != std::string::npos)
                    {
                        size_t b3 = o3 + needle.size();
                        int d3 = 1;
                        bool s3 = false;
                        size_t e3 = b3;
                        for (; e3 < out_line.size(); ++e3)
                        {
                            char c = out_line[e3];
                            if (s3)
                            {
                                if (c == '\\') { ++e3; continue; }
                                if (c == '"') s3 = false;
                                continue;
                            }
                            if (c == '"') { s3 = true; continue; }
                            if (c == '(') ++d3;
                            else if (c == ')') { --d3; if (d3 == 0) break; }
                        }
                        if (e3 < out_line.size())
                            // explicit IInspectable{}: a bare nullptr is C2668
                            // ambiguous against the projected overloads.
                            out_line = out_line.substr(0, b3) + "winrt::IInspectable{nullptr}" + out_line.substr(e3);
                    }
                }
            }
        }
        // R40: SwipeItem.Icon(UIElement...) - projection has no Icon member.
        if (!comment && lo_type == "swipeitem" && lo_prop == "icon")
            comment = true;
        // R41: corrupted template lines ("...{1}.BorderThickness(1)").
        if (!comment && ln.find("}.BorderThickness(") != std::string::npos)
            comment = true;
        // R17g2: Content on ItemContainer (template factory).
        if (!comment && lo_prop == "content" && lo_type == "itemcontainer")
            comment = true;
        // R17e: self-name garbage: var.SymbolIconSource(SymbolIconSource()).
        if (ln.find(".SymbolIconSource(SymbolIconSource())") != std::string::npos)
            comment = true;        // R17e2: any remaining self-call on the wrong receiver (SymbolIconSource
//      on SymbolIconSource etc.) already covered above by R17e.
        // R45: theme/resource lookup results fed to typed setters.
        // theme_lookup_th returns IInspectable; typed setters (Style, Brush,
        // double, hstring...) and property-element setters (IconElement) need
        // the concrete wrapper. Rewrites emit-side helpers missed.
        if (!comment && has_arg && lo_prop != "")
        {
            std::string a45 = trim(arg);
            bool is_lookup = a45.rfind("theme_lookup_th(L\"", 0) == 0;
            bool is_data_tpl = a45.rfind("data_template_th(L\"", 0) == 0;
            if (is_lookup || is_data_tpl)
            {
                // 1) *TemplateSelector setters: DataTemplateSelector wrapper,
                //    not the raw IInspectable/DataTemplate resource.
                if (lo_prop.find("templateselector") != std::string::npos)
                {
                    std::string wrap = "winrt::Microsoft::UI::Xaml::Controls::DataTemplateSelector::wrap_borrowed(" + a45 + ".get_abi())";
                    out_line = out_line.substr(0, out_line.rfind(a45)) + wrap + out_line.substr(out_line.rfind(a45) + a45.size());
                }
                // 2) ItemTemplate/ContentTemplate on ItemsView/ItemsRepeater:
                //    property type is IInspectable, not DataTemplate.
                else if (lo_prop == "itemtemplate" || lo_prop == "contenttemplate")
                {
                    if (lo_type == "itemsview" || lo_type == "itemsrepeater" || lo_type == "scrollviewer")
                    {
                        if (is_data_tpl)
                        {
                            if (lo_type == "itemsview")
                            {
                                // ItemsView.ItemTemplate is IElementFactory-typed in
                                // the WASDK 2.3 metadata. DataTemplate's default
                                // interface IS IElementFactory, so the same raw pointer
                                // reinterpreted via from_abi(+AddRef) is exact — no QI.
                                std::string conv = "winrt::Microsoft::UI::Xaml::IElementFactory::from_abi(" + a45 + ".addref_borrowed())";
                                out_line = out_line.substr(0, out_line.rfind(a45)) + conv + out_line.substr(out_line.rfind(a45) + a45.size());
                            }
                            else
                            {
                                // ItemsRepeater/ScrollViewer ItemTemplates are
                                // IInspectable-typed; every projected class exposes
                                // operator winrt::IInspectable, so a plain conversion
                                // suffices.
                                std::string conv = "winrt::IInspectable(" + a45 + ")";
                                out_line = out_line.substr(0, out_line.rfind(a45)) + conv + out_line.substr(out_line.rfind(a45) + a45.size());
                            }
                        }
                        else if (is_lookup)
                        {
                            // A bare resource lookup yields IInspectable; template-
                            // typed setters need a DataTemplate — re-issue the same
                            // key through data_template_th (which wraps + refcounts).
                            auto kq = a45.find("L\"");
                            if (kq != std::string::npos)
                            {
                                auto ke = a45.find('"', kq + 2);
                                if (ke != std::string::npos)
                                {
                                    std::string conv = "data_template_th(" + a45.substr(kq, ke - kq + 1) + ")";
                                    out_line = out_line.substr(0, out_line.rfind(a45)) + conv + out_line.substr(out_line.rfind(a45) + a45.size());
                                }
                            }
                        }
                    }
                }
                // 3) Icon-typed setters on ButtonBase (FlyoutPresenterStyle etc.
                //    handled by caller paths); Style-typed setters take Style.
                else if (lo_prop == "style" || lo_prop == "itemcontainerstyle"
                    || lo_prop == "flyoutpresenterstyle" || lo_prop == "panebackground")
                {
                    if (lo_prop != "panebackground")
                    {
                        // Resource lookups yield IInspectable; style_from_name takes
                        // an hstring — unbox the lookup result first (C2664 without).
                        std::string conv = "style_from_name(winrt::unbox_value_or<winrt::hstring>(" + a45 + ", winrt::hstring{}))";
                        out_line = out_line.substr(0, out_line.rfind(a45)) + conv + out_line.substr(out_line.rfind(a45) + a45.size());
                    }
                    else
                    {
                        std::string conv = "theme_brush_th(" + a45 + ")";
                        out_line = out_line.substr(0, out_line.rfind(a45)) + conv + out_line.substr(out_line.rfind(a45) + a45.size());
                    }
                }
                // 4) Brush-typed setters (Background/Foreground/BorderBrush
                //    already emit theme_brush_th; catch leftover lookups).
                else if (lo_prop == "background" || lo_prop == "foreground"
                    || lo_prop == "borderbrush")
                {
                    std::string conv = "theme_brush_th(" + a45 + ")";
                    out_line = out_line.substr(0, out_line.rfind(a45)) + conv + out_line.substr(out_line.rfind(a45) + a45.size());
                }
                // 5) hstring-typed setters (Text on TextBlock/TextBox, FontFamily
                //    as string) - bare lookups can't convert to hstring.
                else if (lo_prop == "text")
                {
                    std::string conv = "theme_string_th(" + a45 + ")";
                    out_line = out_line.substr(0, out_line.rfind(a45)) + conv + out_line.substr(out_line.rfind(a45) + a45.size());
                }
            }
        }
        // R46: state-var arguments to typed setters (x:Bind residue).
        // `X.Style(exampleStyle)` where exampleStyle is an hstring state var:
        // wrap with the by-name helpers; literal string args to numeric/struct
        // setters get commented (no conversion exists).
        if (!comment && has_arg)
        {
            std::string a46 = trim(arg);
            if (!a46.empty() && (std::isalpha(static_cast<unsigned char>(a46[0])) || a46[0] == '_'))
            {
                bool plain_id = true;
                for (unsigned char c46 : a46)
                    if (!(std::isalnum(c46) || c46 == '_')) { plain_id = false; break; }
                if (plain_id && !vtype.count(a46))
                {
                    if (lo_prop == "style")
                {
                    std::string conv = "style_from_name(" + a46 + ")";
                    out_line = out_line.substr(0, out_line.rfind(a46)) + conv + out_line.substr(out_line.rfind(a46) + a46.size());
                }
                else if (lo_prop == "text" || lo_prop == "description" || lo_prop == "label")
                {
                    std::string conv = "theme_string_th(" + a46 + ")";
                    out_line = out_line.substr(0, out_line.rfind(a46)) + conv + out_line.substr(out_line.rfind(a46) + a46.size());
                }
                // numeric/struct setters with a string-state var: no C++
                // conversion exists (NumberOfPages, BorderThickness,
                // CornerRadius, VerticalAlignment, Text(TextBox), ...)
                else if (lo_prop == "numberofpages" || lo_prop == "borderthickness"
                    || lo_prop == "cornerradius" || lo_prop == "verticalalignment"
                    || lo_prop == "horizontalalignment" || lo_prop == "margin")
                    comment = true;
                }
            }
        }
        // R50: Command(...) takes Input::ICommand; a theme/resource lookup
        //    (or theme_lookup_th, which yields IInspectable) cannot convert.
        //    Comment the call: the command object is wired from code-behind by
        //    name, not by resource lookup.
        if (!comment && lo_prop == "command" && has_arg)
        {
            std::string a50 = trim(arg);
            // NOTE: resource keys like "CustomXamlUICommand" CONTAIN the
            // substring "ICommand" — only an explicit interface type or a
            // known command factory counts as a valid argument.
            if (a50.find("Input::ICommand") == std::string::npos
                && a50.find("make_relay_command") == std::string::npos)
                comment = true;
        }
        // R51: template-typed properties misattributed to panels/stubs.
        //    StackPanel (and other non-ItemsControl panels) expose no
        //    ZoomedIn*/ZoomedOut*/*ViewTemplate members - the resource refs
        //    were sibling-content in XAML, not real setters (C2039).
        if (!comment && lo_type == "stackpanel"
            && (lo_prop.find("template") != std::string::npos
                || lo_prop == "zoomedin" || lo_prop == "zoomedout"))
            comment = true;
        // R47: known x:Bind-era dead setters (resource keys as values).
        if (!comment && has_arg)
        {
            std::string a47 = trim(arg);
            bool lit47 = a47.size() >= 3 && a47[0] == 'L' && a47[1] == '"' && a47.back() == '"';
            if (lit47)
            {
                std::string v47 = strip_wstr2(a47);
                // PathIcon.Data(L"GitHubIconPath") is a resource-key reference,
                // not literal geometry (literal data contains spaces/commands).
                if (lo_prop == "data" && lo_type == "pathicon"
                    && v47.find(' ') == std::string::npos && v47.find('M') != 0)
                {
                    if (v47 == "GitHubIconPath")
                    {
                        std::string conv = "parse_svg_path(\"M48.854 0C21.839 0 0 22 0 49.217c0 21.756 13.993 40.172 33.405 46.69 2.427.49 3.316-1.059 3.316-2.362 0-1.141-.08-5.052-.08-9.127-13.59 2.934-16.42-5.867-16.42-5.867-2.184-5.704-5.42-7.17-5.42-7.17-4.448-3.015.324-3.015.324-3.015 4.934.326 7.523 5.052 7.523 5.052 4.367 7.496 11.404 5.378 14.235 4.074.404-3.178 1.699-5.378 3.074-6.6-10.839-1.141-22.243-5.378-22.243-24.283 0-5.378 1.94-9.778 5.014-13.2-.485-1.222-2.184-6.275.486-13.038 0 0 4.125-1.304 13.426 5.052a46.97 46.97 0 0 1 12.214-1.63c4.125 0 8.33.571 12.213 1.63 9.302-6.356 13.427-5.052 13.427-5.052 2.67 6.763.97 11.816.485 13.038 3.155 3.422 5.015 7.822 5.015 13.2 0 18.905-11.404 23.06-22.324 24.283 1.78 1.548 3.316 4.481 3.316 9.126 0 6.6-.08 11.897-.08 13.526 0 1.304.89 2.853 3.316 2.364 19.412-6.52 33.405-24.935 33.405-46.691C97.707 22 75.788 0 48.854 0z\")";
                        out_line = out_line.substr(0, out_line.rfind(a47)) + conv + out_line.substr(out_line.rfind(a47) + a47.size());
                    }
                    else
                    {
                        comment = true;
                    }
                }
                // Layout(L"Name"): ItemsRepeater.Layout takes a Layout object.
                else if (lo_prop == "layout" && lo_type == "itemsrepeater")
                    comment = true;
                // ZoomedInTemplate/ZoomedOutTemplate/GroupHeaderTemplate take
                // DataTemplate; a bare resource key never resolves to one.
                else if ((lo_prop == "zoomedintemplate" || lo_prop == "zoomedouttemplate"
                    || lo_prop == "groupheadertemplate" || lo_prop == "itemtemplate")
                    && vtype.count(trim(ln.substr(0, ln.find('.'))))
                    && (lo_type == "semanticzoom" || lo_type == "hierarchicalstatedata"
                        || lo_type == "gridview" || lo_type == "listview" || lo_type == "flipview"))
                    comment = true;
            }
        }
        // R48: collection-typed getters/setters that don't exist on the
        //      projected type (misattributed members on stub/wrong types).
        if (!comment && (lo_prop == "horizontalbartemplate" || lo_prop == "verticalbartemplate"
            || lo_prop == "circulartemplate" || lo_prop == "normalitemtemplate"
            || lo_prop == "accentitemtemplate" || lo_prop == "stringdatatemplate"
            || lo_prop == "intdatatemplate" || lo_prop == "categorytemplate"))
            comment = true; // ItemsRepeater sample's custom template props on StackLayout
        if (!comment && lo_prop == "horizontalswipe" && lo_type == "xamluicommand")
            comment = true; // ListViewItemPresenter-level property, not on XamlUICommand
        if (!comment && lo_prop == "zoomedintemplate" && lo_type == "stackpanel")
            comment = true; // SemanticZoom props emitted on the inner panel
        if (!comment && lo_prop == "zoomedouttemplate" && lo_type == "stackpanel")
            comment = true;
        if (!comment && lo_prop == "zoomedin" && lo_type == "stackpanel")
            comment = true;
        if (!comment && lo_prop == "settingssectionheadertextblockstyle")
            comment = true; // SettingsScreen sample property, not on platform Grid
        if (!comment && lo_prop == "customflyoutpresenterstyle")
            comment = true; // PageHeader sample property, not on platform Grid
        if (!comment && (lo_prop == "exploreritemtemplateselector" || lo_prop == "activityfeedlayout"
            || lo_prop == "sitemplate" || lo_prop == "myfeedlayout"))
            comment = true; // app-defined custom selector/layout types, not projected
        if (!comment && lo_prop == "leftitems" && lo_type == "swipecontrol")
            comment = true; // SwipeItems resource lookups can't be typed
        if (!comment && lo_prop == "rightitems" && lo_type == "swipecontrol")
            comment = true;

        if (comment)
            ln = "// " + out_line;
        else if (out_line != ln)
            ln = out_line;
    }

    for (std::size_t idx = 1; idx < lines.size(); ++idx)
    {
        std::string prev = trim(lines[idx - 1]);
        std::string cur = trim(lines[idx]);
        if (prev.rfind("//", 0) == 0 && cur.rfind("m_bindings.push_back(", 0) == 0)
            lines[idx] = "// " + lines[idx];
    }

    // R45b (post-loop): non-UIElement objects (XAML resources: animations,
    //    converters, template selectors, layouts) misattributed as visual
    //    children cannot be appended to UIElementCollection (C2664). Comment
    //    those appends; the R43 dead-decl pass then removes their factories.
    {
        auto is_non_visual_type = [](std::string const& ty)
        {
            if (ty == "SwitchPresenter" || ty == "ContentPresenter" || ty == "ItemsPresenter") return false;
            return ty.find("Animation") != std::string::npos
                || ty.find("Converter") != std::string::npos
                || (ty.size() >= 16 && ty.compare(ty.size() - 16, 16, "TemplateSelector") == 0)
                || (ty.size() >= 6 && ty.compare(ty.size() - 6, 6, "Layout") == 0)
                || (ty.size() >= 9 && ty.compare(ty.size() - 9, 9, "Presenter") == 0);
        };
        for (auto& lnA : lines)
        {
            std::string tA = trim(lnA);
            if (tA.rfind("//", 0) == 0) continue;
            size_t ap = tA.find(".Append(");
            if (ap == std::string::npos) continue;
            size_t ao = tA.find('(', ap);
            size_t ac = tA.rfind(')');
            if (ao == std::string::npos || ac == std::string::npos || ac <= ao) continue;
            std::string argA = trim(tA.substr(ao + 1, ac - ao - 1));
            while (!argA.empty() && argA.back() == ';') argA.pop_back();
            argA = trim(argA);
            bool nonvisual = false;
            if (!argA.empty() && (std::isalpha(static_cast<unsigned char>(argA[0])) || argA[0] == '_'))
            {
                bool plain = true;
                for (unsigned char cA : argA)
                    if (!(std::isalnum(cA) || cA == '_')) { plain = false; break; }
                if (plain)
                {
                    auto itA = vtype.find(argA);
                    if (itA != vtype.end())
                        nonvisual = is_non_visual_type(itA->second);
                }
                else if (argA.back() == ')')
                {
                    std::string tyA;
                    for (unsigned char cA : argA)
                    {
                        if (std::isalnum(cA) || cA == '_' || cA == ':') tyA.push_back(static_cast<char>(cA));
                        else break;
                    }
                    auto scA = tyA.rfind("::");
                    std::string lastA = (scA == std::string::npos) ? tyA : tyA.substr(scA + 2);
                    nonvisual = is_non_visual_type(lastA);
                }
            }
            if (nonvisual)
                lnA = "// " + lnA;
        }
    }

    // R43 (post-loop): comment dead declarations of unprojected custom types:
    //      auto X = CustomType(); where no non-comment line besides the declaration
    //      itself references X.  Runs after all line rules so commented references
    //      (stubs, skipped x:Bind) don't keep the declaration alive.
    {
        auto line_is_comment = [&](std::string const& l) { std::string tm = trim(l); return tm.rfind("//", 0) == 0; };
        for (auto& ln2 : lines)
        {
            if (line_is_comment(ln2)) continue;
            std::string tm2 = trim(ln2);
            // Factory decls end with ';' (Type();) or rarely ')' - accept both.
            if (tm2.rfind("auto ", 0) != 0 || tm2.find(" = ") == std::string::npos
                || (tm2.back() != ')' && tm2.back() != ';')) continue;
            std::string s3 = tm2.substr(5);
            size_t q3 = 0;
            std::string id3;
            while (q3 < s3.size() && (std::isalnum(static_cast<unsigned char>(s3[q3])) || s3[q3] == '_')) id3.push_back(s3[q3++]);
            if (id3.empty() || id3 == "sv") continue;
            size_t uses = 0;
            for (auto const& l3 : lines)
            {
                if (line_is_comment(l3)) continue;
                std::string::size_type pos = 0;
                while ((pos = l3.find(id3, pos)) != std::string::npos)
                {
                    bool left_ok = (pos == 0) || !(std::isalnum(static_cast<unsigned char>(l3[pos - 1])) || l3[pos - 1] == '_');
                    size_t after = pos + id3.size();
                    bool right_ok = (after >= l3.size()) || !(std::isalnum(static_cast<unsigned char>(l3[after])) || l3[after] == '_');
                    if (left_ok && right_ok) { ++uses; pos = after; }
                    else pos = after;
                }
            }
            if (uses <= 1) ln2 = "// " + ln2;
        }
    }

    // R44 (post-loop): comment corrupted template-factory statements whose
    //      receiver variable was mangled by placeholder substitution, e.g.
    //      "__Grid_winrt::Microsoft::UI::Xaml::Thickness{1}.BorderThickness(1);"
    //      or any "...}.Prop(...)" residue.  Runs last so it catches lines no
    //      matter which emission path produced them.
    for (auto& ln4 : lines)
    {
        std::string t4c = trim(ln4);
        if (t4c.rfind("//", 0) == 0) continue; // comment
        std::string t4 = trim(ln4);
        if (t4.empty()) continue;
        bool corrupt = (t4.rfind("__", 0) == 0 && t4.find("_winrt::") != std::string::npos)
            || t4.find("}.BorderThickness(") != std::string::npos
            || t4.find("}.Children().Append(") != std::string::npos
            || (t4.rfind("auto __", 0) == 0 && t4.find("winrt::") != std::string::npos && t4.find(" = winrt::") == std::string::npos);
        if (corrupt) ln4 = "// " + ln4;
    }

    // Rejoin.
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        result += lines[i];
        result += '\n';
    }
    return result;
}

// True when a raw attribute value is markup we can't evaluate in generated code
// (x:Bind / Binding expressions, DataTemplate serializations).
inline bool is_unbindable_value(std::string const& val)
{
    return val.find("{x:Bind") != std::string::npos
        || val.find("{Binding") != std::string::npos
        || val.find("{binding") != std::string::npos;
}

// 0  PascalCase converter for WinUI projection property/event names
// Property name remapping: winuic modifier names → WinUI3 API names
inline std::string resolve_property_name(std::string_view pk)
{
    // These mappings must stay in sync with the winuic registry.
    // Handle both lowercase (from modifier JSON) and PascalCase (from XAML raw_attrs)
    if (pk == "visible" || pk == "Visible") return "Visibility";
    if (pk == "isEnabled" || pk == "IsEnabled") return "IsEnabled";
    if (pk == "disabled" || pk == "Disabled") return "IsEnabled";
    if (pk == "placeholder" || pk == "Placeholder") return "PlaceholderText";
    if (pk == "selectionMode" || pk == "SelectionMode") return "SelectionMode";
    if (pk == "foregroundColor" || pk == "ForegroundColor") return "Foreground";
    // Slider/NumberBox: Step → StepFrequency
    if (pk == "step" || pk == "Step") return "StepFrequency";
    // ToggleSwitch/CheckBox/RadioButton: isOn → IsOn
    if (pk == "isOn" || pk == "IsOn") return "IsOn";
    // ProgressBar: isIndeterminate → IsIndeterminate
    if (pk == "isIndeterminate" || pk == "IsIndeterminate") return "IsIndeterminate";
    // Modifier names that map to different WinUI3 properties
    if (pk == "bold" || pk == "Bold") return "FontWeight";
    if (pk == "italic" || pk == "Italic") return "FontStyle";
    if (pk == "wrapping" || pk == "Wrapping") return "TextWrapping";
    return std::string(pk);
}

// converts snake_case or lowercase names from layout JSON to PascalCase
inline auto to_pascal(std::string_view s) -> std::string
{
    if (s.empty()) return {};
    // Apply property name remapping first
    std::string mapped = resolve_property_name(s);
    std::string_view src = mapped;
    std::string r;
    r.reserve(src.size());
    bool next_upper = true;
    char prev = 0;
    for (char c : src)
    {
        if (c == '_') { next_upper = true; continue; }
        // Detect camelCase boundaries: lowercase→uppercase transition
        if (prev && std::islower(static_cast<unsigned char>(prev)) && std::isupper(static_cast<unsigned char>(c)))
            next_upper = true;
        if (next_upper) { r += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); next_upper = false; }
        else r += c;
        prev = c;
    }
    return r;
}

// remaps property names that differ between controls.
inline std::string ctrl_prop(std::string const& ctrl, std::string_view pk)
{
    std::string lo_pk(pk);
    for (auto& c : lo_pk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lo_pk == "text" || lo_pk == "content")
    {
        std::string lo_ctrl(ctrl);
        for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_ctrl == "comboboxitem" || lo_ctrl == "listviewitem" || lo_ctrl == "button"
            || lo_ctrl == "togglebutton" || lo_ctrl == "radiobutton" || lo_ctrl == "checkbox"
            || lo_ctrl == "hyperlinkbutton" || lo_ctrl == "appbarbutton" || lo_ctrl == "appbartogglebutton"
            || lo_ctrl == "splitbutton" || lo_ctrl == "togglesplitbutton" || lo_ctrl == "dropdownbutton"
            || lo_ctrl == "menubarelement" || lo_ctrl == "personpicture")
            return "Content";
        if (lo_ctrl == "textblock" || lo_ctrl == "richtextblock" || lo_ctrl == "paragraph"
            || lo_ctrl == "span" || lo_ctrl == "run" || lo_ctrl == "hyperlink"
            || lo_ctrl == "bold" || lo_ctrl == "italic" || lo_ctrl == "underline"
            || lo_ctrl == "menuflyoutitem" || lo_ctrl == "togglemenuflyoutitem"
            || lo_ctrl == "commandbarmenuflyout" || lo_ctrl == "selectorbaritem"
            || lo_ctrl == "radiomenuflyoutitem" || lo_ctrl == "menuflyoutsubitem"
            || lo_ctrl == "menusubitem" || lo_ctrl == "swipeitem"
            || lo_ctrl == "splitmenuflyoutitem" || lo_ctrl == "droppedownitem")
            return "Text";
        if (lo_ctrl == "passwordbox" || lo_ctrl == "textbox" || lo_ctrl == "editbox" || lo_ctrl == "input")
            return "Text";
        return "Content";
    }
    if (lo_pk == "menuitems")
    {
        std::string lo_ctrl(ctrl);
        for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_ctrl == "commandbarflyout") return "PrimaryCommands";
        return "Items";
    }
    if (lo_pk == "items")
    {
        std::string lo_ctrl2(ctrl);
        for (auto& c : lo_ctrl2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_ctrl2 == "commandbar") return "PrimaryCommands";
    }
    if (lo_pk == "elevation")
    {
        return "";
    }
    if (lo_pk == "label")
    {
        std::string lo_ctrl(ctrl);
        for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_ctrl == "toggle" || lo_ctrl == "toggleswitch") return "Header";
        if (lo_ctrl == "checkbox" || lo_ctrl == "radiobutton") return "Content";
    }
    if (lo_pk == "placeholder")
    {
        std::string lo_ctrl(ctrl);
        for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_ctrl == "input" || lo_ctrl == "textbox") return "PlaceholderText";
    }
    if (lo_pk == "inline" || lo_pk == "compact")
    {
        std::string lo_ctrl(ctrl);
        for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_ctrl == "numberbox") return "";
    }
    return to_pascal(pk);
}

// Icon element detection
inline bool is_icon_element(std::string const& name)
{
    return name == "SymbolIcon" || name == "FontIcon" || name == "ImageIcon"
        || name == "PathIcon" || name == "BitmapIcon" || name == "AnimatedIcon";
}

// Controls that accept .Icon(IconElement) property
inline bool control_accepts_icon_prop(std::string const& name)
{
    return name == "NavigationViewItem" || name == "AppBarButton"
        || name == "AppBarToggleButton" || name == "SelectorBarItem"
        || name == "TreeViewNode" || name == "MenuBarItem"
        || name == "NavigationViewItemPresenter"
        || name == "MenuFlyoutItem" || name == "SplitMenuFlyoutItem"
        || name == "ToggleMenuFlyoutItem" || name == "RadioMenuFlyoutItem"
        || name == "MenuFlyoutSubItem" || name == "SwipeItem";
}

// Map common icon kind strings to SymbolIconKind enum values
inline std::string icon_kind_from_string(std::string const& raw)
{
    std::string s = raw;
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// Brush properties that need SolidColorBrush(Colors::Xxx()) instead of L"Xxx"
inline bool is_brush_property(std::string_view pk)
{
    // Accept both camelCase and all-lowercase (caller may or may not lowercase)
    return pk == "background" || pk == "foreground" || pk == "foregroundcolor"
        || pk == "borderbrush" || pk == "fill" || pk == "stroke" || pk == "overlaybrush"
        || pk == "backgroundColor" || pk == "foregroundColor"
        || pk == "borderBrush" || pk == "overlayBrush"
        // Tabular/Charts brush-typed properties
        || pk == "rowbackground" || pk == "alternatingrowbackground"
        || pk == "axislinebrush" || pk == "tickbrush"
        || pk == "gridlinemajorbrush" || pk == "gridlineminorbrush"
        || pk == "datamarkerbrush" || pk == "databrush" || pk == "markerbrush"
        || pk == "legendbrush" || pk == "titlebrush";
}

// Properties whose setter takes IInspectable at the ABI level.
// String values for these must be wrapped in box_value() so they compile.
inline bool is_inspectable_string_prop(std::string_view pk)
{
    // Properties that are IInspectable on MOST controls.
    // Control-specific exceptions are handled in is_inspectable_for_control().
    std::string lo;
    lo.reserve(pk.size());
    for (auto c : pk) lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lo == "content" || lo == "tag" || lo == "header"
        || lo == "selecteditem" || lo == "title"
        || lo == "description" || lo == "watermark"
        || lo == "itemssource" || lo == "selecteditem"
        || lo == "child" || lo == "data";
}

// Controls where "title" is hstring, NOT IInspectable
inline bool title_is_hstring(std::string const& control)
{
    std::string lo = control;
    for (auto& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lo == "menubaritem" || lo == "appbarbutton" || lo == "appbartogglebutton"
        || lo == "selectorbaritem" || lo == "menubaritem" || lo == "navigationviewitem"
        || lo == "navigationviewitempresenter" || lo == "treewinnode"
        || lo == "tabviewitem" || lo == "listviewitem" || lo == "gridviewitem"
        || lo == "comboboxitem" || lo == "radiomenuflyoutitem"
        || lo == "splitmenuflyoutitem" || lo == "menuflyoutitem"
        || lo == "togglemenuflyoutitem" || lo == "menuflyoutsubitem"
        || lo == "titlebar";
}

// Control-aware: should we box_value for this prop+control combo?
inline bool needs_box_value(std::string const& control, std::string_view pk)
{
    if (!is_inspectable_string_prop(pk)) return false;
    std::string lo_pk;
    lo_pk.reserve(pk.size());
    for (auto c : pk) lo_pk += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // "title" is hstring on menu items, appbar buttons, list/grid items
    if (lo_pk == "title" && title_is_hstring(control)) return false;
    // "header" is hstring on ListViewItem, ComboBoxItem, GridViewItem
    if (lo_pk == "header") {
        std::string lo_ctrl = control;
        for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_ctrl == "listviewitem" || lo_ctrl == "comboboxitem" || lo_ctrl == "gridviewitem")
            return false;
    }
    // "description" is hstring on XamlUICommand
    if (lo_pk == "description") {
            std::string lo_ctrl = control;
            for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lo_ctrl == "xamluicommand")
                return false;
        }
        // Charts *Series.Title takes HSTRING (IDL: [propput] Title([in] HSTRING)),
        // never IInspectable — boxing it is a C2665.
        if (lo_pk == "title") {
            std::string lo_ctrl = control;
            for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto sc = lo_ctrl.rfind("::");
            if (sc != std::string::npos) lo_ctrl = lo_ctrl.substr(sc + 2);
            if (lo_ctrl.size() >= 6 && lo_ctrl.compare(lo_ctrl.size() - 6, 6, "series") == 0)
                return false;
        }
        return true;
    }

// Wrap a string/numeric value in box_value() for IInspectable-typed properties
// If the value is already a non-string expression (enum, object ctor), returns it unchanged
// Legacy overload (no control context) - falls back to is_inspectable_string_prop
inline std::string box_if_inspectable(std::string_view pk, std::string const& val)
{
    if (!is_inspectable_string_prop(pk)) return val;
    if (val.find("::") != std::string::npos) return val;
    // Bool values: emit raw true/false (WinRT accepts bool directly)
    if (val == "true" || val == "false") return val;
    if (val == "nullptr") return val;
    // x:Bind broken binding expressions -> nullptr
    if (val.find("x:Bind") != std::string::npos) return "nullptr";
    // Already an L"..." or "..." string literal — wrap in box_value BEFORE checking parens
    // so strings like L"System Backdrops (Mica/Acrylic)" are boxed properly.
    bool already_wstr = (val.size() > 2 && val[0] == 'L' && val[1] == '"' && val.back() == '"');
    if (already_wstr) return "winrt::box_value(" + val + ")";
    bool already_str = (val.size() >= 2 && val.front() == '"' && val.back() == '"');
    if (already_str) return "winrt::box_value(" + val + ")";
    if (val.find("(") != std::string::npos && val.find(")") != std::string::npos) return val;
    if (val.find("box_value") != std::string::npos) return val;
    if (val.find("make_solid_brush") != std::string::npos) return val;
    if (val.find("Brush::from_abi") != std::string::npos) return val;
    if (val.find("from_abi") != std::string::npos) return val;
    bool is_num = !val.empty() && (std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-');
    if (is_num) return "winrt::box_value(" + val + ")";
    return "winrt::box_value(L\"" + val + "\")";
}

// Control-aware overload: checks if the property actually needs boxing for this control
inline std::string box_if_inspectable(std::string const& control, std::string_view pk, std::string const& val)
{
    // Charts *Series.Title is HSTRING: never box, but widen bare literals
    // (a raw 2025 would otherwise be emitted as Title(2025)).
    {
        std::string lo_pk_t(pk);
        for (auto& c : lo_pk_t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_pk_t == "title")
        {
            std::string lo_ctrl_t(control);
            for (auto& c : lo_ctrl_t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto sc = lo_ctrl_t.rfind("::");
            if (sc != std::string::npos) lo_ctrl_t = lo_ctrl_t.substr(sc + 2);
            if (lo_ctrl_t.size() >= 6 && lo_ctrl_t.compare(lo_ctrl_t.size() - 6, 6, "series") == 0)
            {
                if (val.size() > 2 && val[0] == 'L' && val[1] == '"') return val;
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') return val;
                if (val.find("::") != std::string::npos || val.find('(') != std::string::npos) return val;
                return "L\"" + val + "\"";
            }
        }
    }
    // IsChecked takes IReference<bool> — bools need boxing + QI
    {
        std::string lo_pk_bi(pk);
        for (auto& c : lo_pk_bi) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_pk_bi == "ischecked" && (val == "true" || val == "false")) {
            // box_value(bool) returns void* in this projection — construct IReference from the raw pointer
            // menu flyout items (RadioMenuFlyoutItem/ToggleMenuFlyoutItem) take plain bool
            if (is_plain_bool_ischecked(control))
                return val;
            // box_value(bool) returns void* in this projection - construct IReference from the raw pointer
            return "winrt::Windows::Foundation::IReference<bool>(winrt::box_value(" + val + "))";
        }
        if ((lo_pk_bi == "oncontent" || lo_pk_bi == "offcontent") && (val == "true" || val == "false"))
            return "winrt::box_value(" + val + ")";
    }
    // FontWeight/FontStyle setters take the real projected structs/enums
    // (the WASDK metadata contract the Microsoft.UI.Xaml setters consume).
    // A boxed number cannot feed those typed setters (C2665) — map the raw
    // numeric to the exact struct/enum the setter declares.
    {
        std::string lo_pk_f(pk);
        for (auto& c : lo_pk_f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool is_num_f = !val.empty() && (std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-');
        if (is_num_f && (lo_pk_f == "fontweight" || lo_pk_f == "bold")) {
            int w = std::atoi(val.c_str());
            if (w <= 0) w = 400;
            return "winrt::Windows::UI::Text::FontWeight{" + std::to_string(w) + "}";
        }
        if (is_num_f && (lo_pk_f == "fontstyle" || lo_pk_f == "italic")) {
            return (val == "0") ? "winrt::Windows::UI::Text::FontStyle::Normal"
                                : "winrt::Windows::UI::Text::FontStyle::Italic";
        }
    }
    if (!needs_box_value(control, pk)) return val;
    if (val.find("::") != std::string::npos) return val;
    // Bool values: emit raw true/false (WinRT accepts bool directly)
    if (val == "true" || val == "false") return val;
    if (val == "nullptr") return val;
    // x:Bind broken binding expressions -> nullptr
    if (val.find("x:Bind") != std::string::npos) return "nullptr";
    // Already an L"..." or "..." string literal — wrap in box_value BEFORE checking parens
    // so strings like L"System Backdrops (Mica/Acrylic)" are boxed properly.
    bool already_wstr = (val.size() > 2 && val[0] == 'L' && val[1] == '"' && val.back() == '"');
    if (already_wstr) return "winrt::box_value(" + val + ")";
    bool already_str = (val.size() >= 2 && val.front() == '"' && val.back() == '"');
    if (already_str) return "winrt::box_value(" + val + ")";
    if (val.find("(") != std::string::npos && val.find(")") != std::string::npos) return val;
    if (val.find("box_value") != std::string::npos) return val;
    if (val.find("make_solid_brush") != std::string::npos) return val;
    if (val.find("Brush::from_abi") != std::string::npos) return val;
    if (val.find("from_abi") != std::string::npos) return val;
    bool is_num = !val.empty() && (std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-');
    if (is_num) return "winrt::box_value(" + val + ")";
    return "winrt::box_value(L\"" + val + "\")";
}

// Named colors that map to Windows::UI::Color via Colors:: helper
inline bool is_known_color(std::string const& v)
{
    static const char* names[] = {
        "Transparent", "White", "Black", "Red", "Green", "Blue",
        "Yellow", "Orange", "Gray", "Cyan", "Magenta", "Brown",
        "AliceBlue", "AntiqueWhite", "Aqua", "Aquamarine", "Azure",
        "Beige", "Bisque", "BlanchedAlmond", "BlueViolet", "BurlyWood",
        "CadetBlue", "Chartreuse", "Chocolate", "Coral", "CornflowerBlue",
        "Cornsilk", "Crimson", "DarkBlue", "DarkCyan", "DarkGoldenrod",
        "DarkGray", "DarkGreen", "DarkKhaki", "DarkMagenta", "DarkOliveGreen",
        "DarkOrange", "DarkOrchid", "DarkRed", "DarkSalmon", "DarkSeaGreen",
        "DarkSlateBlue", "DarkSlateGray", "DarkTurquoise", "DarkViolet",
        "DeepPink", "DeepSkyBlue", "DimGray", "DodgerBlue", "FireBrick",
        "Gold", "Goldenrod", "GreenYellow", "Honeydew",
        "HotPink", "IndianRed", "Indigo", "Ivory", "Khaki", "Lavender",
        "Linen", "Maroon", "Navy", "Olive", "OliveDrab", "OrangeRed",
        "Orchid", "Pink", "Plum", "Purple", "Salmon", "Silver",
        "SkyBlue", "SlateGray", "Snow", "SteelBlue", "Tan", "Teal",
        "Thistle", "Tomato", "Turquoise", "Violet", "Wheat",
        "WhiteSmoke", "YellowGreen", nullptr
    };
    for (int i = 0; names[i]; ++i)
        if (v == names[i]) return true;
    return false;
}

// Convert hex color (#AARRGGBB or #RRGGBB) to comma-separated argb/rgb
inline std::string hex_to_argb(std::string const& hex)
{
    if (hex.size() != 9 || hex[0] != '#') return "0, 0, 0, 0";
    auto h = [&](int i) { return (int)std::stoul(hex.substr(i, 2), nullptr, 16); };
    return std::to_string(h(1)) + ", " + std::to_string(h(3)) + ", " + std::to_string(h(5)) + ", " + std::to_string(h(7));
}
inline std::string hex_to_rgb(std::string const& hex)
{
    if (hex.size() != 7 || hex[0] != '#') return "0, 0, 0";
    auto h = [&](int i) { return (int)std::stoul(hex.substr(i, 2), nullptr, 16); };
    return std::to_string(h(1)) + ", " + std::to_string(h(3)) + ", " + std::to_string(h(5));
}

// ── CSS color format converters (compile-time, no WinUI runtime dependency) ──
// Convert hsl(h, s%, l%) or hsla(h, s%, l%, a) to "A, R, G, B" for ColorHelper::FromArgb
inline std::string hsl_to_argb(int h, int s, int l, int a = 255)
{
    // HSL to RGB via standard algorithm
    double hd = (h % 360) / 360.0;
    double sd = s / 100.0;
    double ld = l / 100.0;
    auto hue2rgb = [](double p, double q, double t) -> double {
        if (t < 0) t += 1; if (t > 1) t -= 1;
        if (t < 1.0/6) return p + (q - p) * 6 * t;
        if (t < 1.0/2) return q;
        if (t < 2.0/3) return p + (q - p) * (2.0/3 - t) * 6;
        return p;
    };
    double r, g, b;
    if (sd == 0) { r = g = b = ld; }
    else {
        double q = ld < 0.5 ? ld * (1 + sd) : ld + sd - ld * sd;
        double p = 2 * ld - q;
        r = hue2rgb(p, q, hd + 1.0/3);
        g = hue2rgb(p, q, hd);
        b = hue2rgb(p, q, hd - 1.0/3);
    }
    return std::to_string(a) + ", " + std::to_string((int)(r*255+0.5)) + ", " + std::to_string((int)(g*255+0.5)) + ", " + std::to_string((int)(b*255+0.5));
}
// Convert color(display-p3, r, g, b) or color(display-p3, r, g, b, a) to "A, R, G, B"
// P3 values are 0.0-1.0, sRGB gamut — linear conversion for display-p3
inline std::string p3_to_argb(double r, double g, double b, double a = 1.0)
{
    // Display P3 to sRGB (simplified: clamp to sRGB gamut)
    // Full conversion would use the P3→XYZ→sRGB matrix, but for codegen
    // we use the linear approximation since most P3 colors are within sRGB.
    auto clamp255 = [](double v) { return (int)(std::max(0.0, std::min(1.0, v)) * 255 + 0.5); };
    return std::to_string(clamp255(a)) + ", " + std::to_string(clamp255(r)) + ", " + std::to_string(clamp255(g)) + ", " + std::to_string(clamp255(b));
}
// Convert oklch(L, C, H) or oklch(L, C, H, A) to "A, R, G, B"
// OKLCH: L=0-1, C=0-~0.4, H=0-360
inline std::string oklch_to_argb(double L, double C, double H, double A = 1.0)
{
    // OKLCH → OKLab → linear sRGB → sRGB (via Björn Ottosson's conversion)
    double hrad = H * 3.14159265358979 / 180.0;
    double a = C * std::cos(hrad);
    double b = C * std::sin(hrad);
    // OKLab to LMS
    double l_ = L + 0.3963377774 * a + 0.2158037573 * b;
    double m_ = L - 0.1055613458 * a - 0.0638541728 * b;
    double s_ = L - 0.0894841775 * a - 1.2914855480 * b;
    double l = l_*l_*l_, m = m_*m_*m_, s = s_*s_*s_;
    // LMS to linear sRGB
    double r = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    double bl = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    // Linear to sRGB
    auto to_srgb = [](double v) -> int {
        v = std::max(0.0, std::min(1.0, v));
        return (int)((v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0/2.4) - 0.055) * 255 + 0.5);
    };
    return std::to_string((int)(A*255+0.5)) + ", " + std::to_string(to_srgb(r)) + ", " + std::to_string(to_srgb(g)) + ", " + std::to_string(to_srgb(bl));
}
// Detect CSS color format and return C++ FromArgb(...) expression, or empty if not CSS
inline std::string try_css_color(std::string const& val)
{
    std::string v = val;
    // Strip L"..." or "..." wrappers
    if (v.size() > 3 && v[0] == 'L' && v[1] == '"') v = v.substr(2, v.size() - 3);
    else if (v.size() > 2 && v[0] == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    // hsla(h, s%, l%, a) or hsl(h, s%, l%)
    if (v.size() > 4 && (v.substr(0,4) == "hsl(" || v.substr(0,5) == "hsla("))
    {
        bool is_hsla = v[3] == 'a';
        auto inner = v.find('(');
        auto close = v.rfind(')');
        if (inner == std::string::npos || close == std::string::npos) return {};
        std::string args = v.substr(inner+1, close-inner-1);
        // Parse: h, s%, l%[, a]
        int h=0, s=50, l=50, a=255;
        // Simple comma/space parse
        std::vector<std::string> parts;
        std::string cur;
        for (char c : args) { if (c==',' || c==' ') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) parts.push_back(cur);
        if (parts.size() >= 3) {
            h = (int)std::stod(parts[0]);
            std::string sp = parts[1]; if (!sp.empty() && sp.back()=='%') sp.pop_back(); s = (int)std::stod(sp);
            std::string lp = parts[2]; if (!lp.empty() && lp.back()=='%') lp.pop_back(); l = (int)std::stod(lp);
        }
        if (is_hsla && parts.size() >= 4) {
            double ad = std::stod(parts[3]); if (ad <= 1.0) a = (int)(ad*255+0.5); else a = (int)ad;
        }
        return "winrt::Windows::UI::Color{" + hsl_to_argb(h, s, l, a) + "}";
    }
    // color(display-p3, r, g, b[, a])
    if (v.size() > 17 && v.substr(0,17) == "color(display-p3,")
    {
        auto inner = v.find('(');
        auto close = v.rfind(')');
        if (inner == std::string::npos || close == std::string::npos) return {};
        std::string args = v.substr(inner+1, close-inner-1);
        std::vector<std::string> parts;
        std::string cur;
        for (char c : args) { if (c==',') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) parts.push_back(cur);
        // skip first token ("display-p3")
        std::vector<std::string> rgb;
        for (size_t i = 1; i < parts.size(); ++i) {
            std::string p = parts[i]; while (!p.empty() && p[0]==' ') p.erase(0,1);
            rgb.push_back(p);
        }
        double r=0, g=0, b=0, a=1.0;
        if (rgb.size() >= 3) { r = std::stod(rgb[0]); g = std::stod(rgb[1]); b = std::stod(rgb[2]); }
        if (rgb.size() >= 4) a = std::stod(rgb[3]);
        return "winrt::Windows::UI::Color{" + p3_to_argb(r, g, b, a) + "}";
    }
    // oklch(L, C, H[, A])
    if (v.size() > 6 && v.substr(0,6) == "oklch(")
    {
        auto inner = v.find('(');
        auto close = v.rfind(')');
        if (inner == std::string::npos || close == std::string::npos) return {};
        std::string args = v.substr(inner+1, close-inner-1);
        std::vector<std::string> parts;
        std::string cur;
        for (char c : args) { if (c==',') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) parts.push_back(cur);
        double L=0.5, C=0.1, H=0, A=1.0;
        if (parts.size() >= 3) {
            std::string p0 = parts[0]; while (!p0.empty() && p0[0]==' ') p0.erase(0,1);
            std::string p1 = parts[1]; while (!p1.empty() && p1[0]==' ') p1.erase(0,1);
            std::string p2 = parts[2]; while (!p2.empty() && p2[0]==' ') p2.erase(0,1);
            L = std::stod(p0); C = std::stod(p1); H = std::stod(p2);
        }
        if (parts.size() >= 4) { std::string pa = parts[3]; while (!pa.empty() && pa[0]==' ') pa.erase(0,1); A = std::stod(pa); }
        return "winrt::Windows::UI::Color{" + oklch_to_argb(L, C, H, A) + "}";
    }
    return {};
}

// Resolve property-specific string values to compilable C++ expressions
// Returns empty string if no special handling needed (caller wraps in L"...")

// Escape a C++ string literal: replace newlines, tabs, backslashes, quotes
inline std::string escape_cpp_string(std::string const& s) {
    std::string r;
    for (char c : s) {
        if (c == '\n') { r += "\\n"; }
        else if (c == '\r') { r += "\\r"; }
        else if (c == '\t') { r += "\\t"; }
        else if (c == '\\') { r += "\\\\"; }
        else if (c == '"') { r += "\""; }
        else { r += c; }
    }
    return r;
}

inline std::string resolve_prop_value(std::string_view pk, std::string const& val)
{
    // Lowercase key for case-insensitive matching
    std::string pk_lo; pk_lo.reserve(pk.size());
    for (auto c : pk) pk_lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool is_vert = (pk_lo == "verticalalignment" || pk_lo == "verticalcontentalignment");
    bool is_horz = (pk_lo == "horizontalalignment" || pk_lo == "horizontalcontentalignment" || pk_lo == "alignment");
    if (is_vert || is_horz)
    {
        std::string ns = is_horz
            ? "winrt::Microsoft::UI::Xaml::HorizontalAlignment::"
            : "winrt::Microsoft::UI::Xaml::VerticalAlignment::";
        if (val == "Center") return ns + "Center";
        if (val == "Top") return ns + "Top";
        if (val == "Bottom") return ns + "Bottom";
        if (val == "Stretch") return ns + "Stretch";
        if (val == "Left" || val == "Start") return ns + "Left";
        if (val == "Right" || val == "End") return ns + "Right";
    }
    // StackPanel/VariableSizedWrapGrid Orientation enum
    if (pk_lo == "orientation")
    {
        if (val == "Horizontal") return "winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal";
        if (val == "Vertical") return "winrt::Microsoft::UI::Xaml::Controls::Orientation::Vertical";
    }
    // Spacing enum / resource keys
    if (pk_lo == "spacing" || pk_lo == "linespacing" || pk_lo == "rowspacing" || pk_lo == "columnspacing")
    {
        std::string v = val;
        while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\r' || v.front() == '\n')) v.erase(0, 1);
        if (v == "LayoutSpacing") return "24.0";
        if (v == "ElementSpacing") return "16.0";
        if (v == "CardSpacing") return "12.0";
        bool is_numeric = !v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-');
        if (is_numeric) return v;
        return "winrt::unbox_value_or<double>(theme_lookup_th(L\"" + v + "\", winrt::box_value(0.0)), 0.0)";
    }
    // NavigateUri / UriSource: Uri-typed props — wrap string in winrt::Windows::Foundation::Uri
    if (pk_lo == "navigateuri" || pk_lo == "urisource")
    {
        // Markup values ({x:Bind X}, {StaticResource ...}, {ThemeResource ...})
        // must NOT become Uri literals: the WinRT Uri constructor THROWS on
        // invalid strings, so a literal L"{x:Bind Link}" killed the process
        // mid-BuildUI (silent death, no crash reporter). Emit a default (null)
        // Uri instead — null is valid ABI for these nullable object-reference
        // properties and matches "no value set" until a real binding exists.
        if (val.find('{') != std::string::npos)
            return "winrt::Windows::Foundation::Uri{}";
        std::string clean = val;
        if (clean.size() > 2 && clean[0] == 'L' && clean[1] == '\"')
            clean = clean.substr(2, clean.size() - 3);
        else if (clean.size() >= 2 && clean.front() == '\"' && clean.back() == '\"')
            clean = clean.substr(1, clean.size() - 2);
        // strip trailing () artifact
        while (clean.size() >= 2 && clean.back() == ')' && clean[clean.size()-2] == '(')
            clean.pop_back(), clean.pop_back();
        if (!clean.empty())
            return "winrt::Windows::Foundation::Uri(L\"" + clean + "\")";
    }
    // Corner-radius struct props: a bare value here is a resource key
    // ({ThemeResource ControlCornerRadius} arrives markup-stripped). Baking the
    // WinUI default radius as a literal keeps the shape without a resource
    // round-trip at runtime.
    if (pk_lo == "cornerradius")
    {
        bool numeric = !val.empty() && (std::isdigit(static_cast<unsigned char>(val[0])) || val[0] == '-');
        if (!numeric && val != "nullptr" && val.find('(') == std::string::npos && val.find('{') == std::string::npos)
            return "winrt::Microsoft::UI::Xaml::CornerRadius{4, 4, 4, 4}";
    }
    // FontWeight: SemiBold->600 etc. FontWeight takes the FontWeight struct
    if (pk_lo == "fontweight" || pk_lo == "bold")
    {
        if (val == "Thin") return "winrt::Windows::UI::Text::FontWeight{100}";
        if (val == "ExtraLight" || val == "UltraLight") return "winrt::Windows::UI::Text::FontWeight{200}";
        if (val == "Light") return "winrt::Windows::UI::Text::FontWeight{300}";
        if (val == "Normal" || val == "Regular") return "winrt::Windows::UI::Text::FontWeight{400}";
        if (val == "Medium") return "winrt::Windows::UI::Text::FontWeight{500}";
        if (val == "SemiBold" || val == "DemiBold") return "winrt::Windows::UI::Text::FontWeight{600}";
        if (val == "Bold" || val == "true") return "winrt::Windows::UI::Text::FontWeight{700}";
        if (val == "ExtraBold" || val == "UltraBold") return "winrt::Windows::UI::Text::FontWeight{800}";
        if (val == "Black" || val == "Heavy") return "winrt::Windows::UI::Text::FontWeight{900}";
        if (val == "ExtraBlack" || val == "UltraBlack") return "winrt::Windows::UI::Text::FontWeight{950}";
    }

    // ScrollBarVisibility enum
    if (pk_lo == "horizontalscrollbarvisibility" || pk_lo == "verticalscrollbarvisibility"
        || pk_lo == "horizontalscrollbarsvisibility" || pk_lo == "verticalscrollbarsvisibility")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Auto";
        if (val == "Hidden") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Hidden";
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled";
        if (val == "Visible") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Visible";
    }
    // ScrollMode enum
    if (pk_lo == "horizontalscrollmode" || pk_lo == "verticalscrollmode")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Auto";
        if (val == "Enabled") return "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Enabled";
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Disabled";
    }
    // ScrollBarVisibility (generic property name)
    if (pk_lo == "scrollbarvisibility")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Auto";
        if (val == "Hidden") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Hidden";
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled";
        if (val == "Visible") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Visible";
    }
    // Symbol enum (for SymbolIcon)
    if (pk_lo == "symbol")
    {
        // Emit as integer cast — Symbol enum may not be projected
        static const std::map<std::string, int> symbols = {
            {"Accept",100}, {"Add",101}, {"Back",102}, {"Cancel",103},
            {"CheckMark",107}, {"Clear",116}, {"Close",118}, {"Copy",122},
            {"Delete",125}, {"Edit",128}, {"Find",133}, {"Forward",134},
            {"Help",137}, {"Home",138}, {"Like",139}, {"Mail",140},
            {"MediaPlay",142}, {"MediaPause",143}, {"MediaStop",144},
            {"More",146}, {"Move",147}, {"None",0}, {"Redo",154},
            {"Save",159}, {"Search",161}, {"Send",162}, {"Settings",163},
            {"Share",164}, {"Sort",166}, {"Sync",167}, {"Undo",170},
            {"Zoom",172}, {"Ringer",62242}, {"Clock",57642},
            {"EmojiFrown",57655}, {"EmojiSmile",57656},
        };
        auto it = symbols.find(val);
        if (it != symbols.end()) return "static_cast<int>(" + std::to_string(it->second) + ")";
    }
    // SpinButtonPlacementMode enum
    if (pk_lo == "spinbuttonplacementmode")
    {
        if (val == "Compact") return "winrt::Microsoft::UI::Xaml::Controls::NumberBoxSpinButtonPlacementMode::Compact";
        if (val == "Inline") return "winrt::Microsoft::UI::Xaml::Controls::NumberBoxSpinButtonPlacementMode::Inline";
        if (val == "Hidden") return "winrt::Microsoft::UI::Xaml::Controls::NumberBoxSpinButtonPlacementMode::Hidden";
    }
    // NumberBox.ValidationMode enum — raw attrs can arrive L"-quoted; the
    // setter takes NumberBoxValidationMode, never an hstring (C2665).
    if (pk_lo == "validationmode")
    {
        std::string v = val;
        if (v.size() > 2 && v[0] == 'L' && v[1] == '\"') v = v.substr(2, v.size() - 3);
        else if (v.size() >= 2 && v.front() == '\"' && v.back() == '\"') v = v.substr(1, v.size() - 2);
        if (v == "InvalidInputOverwritten")
            return "winrt::Microsoft::UI::Xaml::Controls::NumberBoxValidationMode::InvalidInputOverwritten";
        if (v == "InvalidInputRevealsValue")
            return "winrt::Microsoft::UI::Xaml::Controls::NumberBoxValidationMode::InvalidInputRevealsValue";
    }
    // InfoBarSeverity enum — also handle lowercase values
    if (pk_lo == "severity")
    {
        std::string lo_val = val;
        for (auto& c : lo_val) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_val == "informational" || lo_val == "info") return "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational";
        if (lo_val == "success") return "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success";
        if (lo_val == "warning") return "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning";
        if (lo_val == "error") return "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error";
    }
    // CheckBox IsChecked: bool -> IReference<bool> (projection takes IReference<bool>)
    if (pk_lo == "ischecked" && (val == "true" || val == "false" || val == "True" || val == "False"))
    {
        std::string b = (val == "true" || val == "True") ? "true" : "false";
        // box_value(bool) returns void* in this projection — construct IReference from the raw pointer
        return "winrt::Windows::Foundation::IReference<bool>(winrt::box_value(" + b + "))";
    }
    // ElementSoundMode enum
    if (pk_lo == "elementsoundmode")
    {
        if (val == "Default") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::Default";
        if (val == "FocusOnly") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::FocusOnly";
        if (val == "ProgramOnly") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::ProgramOnly";
        if (val == "SystemOnly") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::SystemOnly";
        if (val == "Off") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::Off";
    }
    // SliderSnapsTo enum
    if (pk_lo == "slidersnapsto")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::None";
        if (val == "Ticks") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::Ticks";
        if (val == "TickMarks") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::TickMarks";
        if (val == "Value") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::Value";
    }
    // TickPlacement enum
    if (pk_lo == "tickplacement")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::None";
        if (val == "TopLeft") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::TopLeft";
        if (val == "BottomRight") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::BottomRight";
        if (val == "Both") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::Both";
        if (val == "Outside") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::Outside";
        if (val == "Inline") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::Inline";
    }
    // SliderSnapsTo enum
    if (pk_lo == "slidersnapsto")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::None";
        if (val == "Ticks") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::Ticks";
        if (val == "TickMarks") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::TickMarks";
        if (val == "Value") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::Value";
    }
    // TickPlacement enum
    if (pk_lo == "tickplacement")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::None";
        if (val == "TopLeft") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::TopLeft";
        if (val == "BottomRight") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::BottomRight";
        if (val == "Both") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::Both";
        if (val == "Outside") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::Outside";
        if (val == "Inline") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::TickPlacement::Inline";
    }
    // TextWrapping enum
    if (pk_lo == "textwrapping" || pk_lo == "wrapping")
    {
        if (val == "Wrap" || val == "wrap" || val == "true") return "winrt::Microsoft::UI::Xaml::TextWrapping::Wrap";
        if (val == "NoWrap" || val == "nowrap" || val == "false") return "winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap";
        if (val == "WrapWholeWords" || val == "wrapwholewords") return "winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords";
        return "winrt::Microsoft::UI::Xaml::TextWrapping::Wrap";
    }
    // FontStyle enum — takes Windows::UI::Text::FontStyle
    if (pk_lo == "fontstyle" || pk_lo == "italic")
    {
        if (val == "Italic" || val == "true") return "winrt::Windows::UI::Text::FontStyle::Italic";
        if (val == "Normal" || val == "false") return "winrt::Windows::UI::Text::FontStyle::Normal";
        if (val == "Oblique") return "winrt::Windows::UI::Text::FontStyle::Oblique";
    }
    // Style property: {StaticResource Key} -> guarded Application.Resources lookup
    // (.as<Style>() on the found IInspectable). Bare style names -> skip the
    // setter entirely: a default-constructed Style() is rejected by XAML with
    // E_FAIL (fail-fast 0xC000027B) and the default theme already styles the
    // control, so emitting nothing is both safe and visually equivalent.
    if (pk_lo == "style")
    {
        std::string v = val;
        auto sr = v.find("{StaticResource ");
        if (sr != std::string::npos)
        {
            auto end = v.find('}', sr);
            if (end != std::string::npos)
                v = v.substr(sr + 16, end - sr - 16);
        }
        else if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        if (!v.empty())
        {
            if (v == "accent" || v == "Accent") v = "AccentButtonStyle";
            return "style_lookup_th(L\"" + v + "\")";
        }
        return ""; // skip - no empty Style() placeholder
    }
    // Visibility enum
    if (pk_lo == "visibility")
    {
        if (val == "Visible") return "winrt::Microsoft::UI::Xaml::Visibility::Visible";
        if (val == "Collapsed") return "winrt::Microsoft::UI::Xaml::Visibility::Collapsed";
    }
    // FlowDirection enum
    if (pk_lo == "flowdirection")
    {
        if (val == "LeftToRight") return "winrt::Microsoft::UI::Xaml::FlowDirection::LeftToRight";
        if (val == "RightToLeft") return "winrt::Microsoft::UI::Xaml::FlowDirection::RightToLeft";
    }
    // RequestedTheme enum
    if (pk_lo == "requestedtheme")
    {
        if (val == "Light") return "winrt::Microsoft::UI::Xaml::ElementTheme::Light";
        if (val == "Dark") return "winrt::Microsoft::UI::Xaml::ElementTheme::Dark";
        if (val == "Default") return "winrt::Microsoft::UI::Xaml::ElementTheme::Default";
    }
    // TextLineBounds enum
    if (pk_lo == "textlinebounds")
    {
        if (val == "TrimToCapHeight") return "winrt::Microsoft::UI::Xaml::TextLineBounds::TrimToCapHeight";
        if (val == "TrimToBaseline") return "winrt::Microsoft::UI::Xaml::TextLineBounds::TrimToBaseline";
        if (val == "TrimToLeadingBaseline") return "winrt::Microsoft::UI::Xaml::TextLineBounds::TrimToLeadingBaseline";
        if (val == "Full") return "winrt::Microsoft::UI::Xaml::TextLineBounds::Full";
    }
    // DefaultLabelPosition enum
    if (pk_lo == "defaultlabelposition")
    {
        if (val == "Right") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Right";
        if (val == "Left") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Left";
        if (val == "Top") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Top";
        if (val == "Collapsed") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Collapsed";
        if (val == "Bottom") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Bottom";
    }
    // ScrollingIndicatorMode enum
    if (pk_lo == "indicatormode")
    {
        if (val == "MouseIndicator") return "winrt::Microsoft::UI::Xaml::Controls::ScrollingIndicatorMode::MouseIndicator";
        if (val == "KeyboardIndicator") return "winrt::Microsoft::UI::Xaml::Controls::ScrollingIndicatorMode::KeyboardIndicator";
        if (val == "MouseOrKeyboardIndicator") return "winrt::Microsoft::UI::Xaml::Controls::ScrollingIndicatorMode::MouseOrKeyboardIndicator";
    }
    // ScrollViewContentOrientation enum
    if (pk_lo == "contentorientation")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::ScrollViewContentOrientation::None";
        if (val == "Horizontal") return "winrt::Microsoft::UI::Xaml::Controls::ScrollViewContentOrientation::Horizontal";
        if (val == "Vertical") return "winrt::Microsoft::UI::Xaml::Controls::ScrollViewContentOrientation::Vertical";
        if (val == "Both") return "winrt::Microsoft::UI::Xaml::Controls::ScrollViewContentOrientation::Both";
    }
    // ZoomMode enum
    if (pk_lo == "zoommode")
    {
        if (val == "Enabled") return "winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Enabled";
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Disabled";
    }
    // PasswordRevealMode enum
    if (pk_lo == "passwordrevealmode")
    {
        if (val == "Hidden") return "winrt::Microsoft::UI::Xaml::Controls::PasswordRevealMode::Hidden";
        if (val == "Visible") return "winrt::Microsoft::UI::Xaml::Controls::PasswordRevealMode::Visible";
        if (val == "Peeking") return "winrt::Microsoft::UI::Xaml::Controls::PasswordRevealMode::Peeking";
    }
    // FlyoutPlacementMode enum (preferredplacement and placement)
    if (pk_lo == "preferredplacement" || pk_lo == "placement")
    {
        if (val == "Top") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top";
        if (val == "Bottom") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Bottom";
        if (val == "Left") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Left";
        if (val == "Right") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Right";
        if (val == "Full") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Full";
        if (val == "TopEdgeAlignedLeft") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::TopEdgeAlignedLeft";
        if (val == "TopEdgeAlignedRight") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::TopEdgeAlignedRight";
        if (val == "BottomEdgeAlignedLeft") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft";
        if (val == "BottomEdgeAlignedRight") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight";
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Auto";
    }
    // KeyboardAcceleratorPlacementMode enum
    if (pk_lo == "keyboardacceleratorplacementmode")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::KeyboardAcceleratorPlacementMode::Auto";
        if (val == "Hidden") return "winrt::Microsoft::UI::Xaml::Controls::KeyboardAcceleratorPlacementMode::Hidden";
    }
    // DisplayMode (NavigationView) enum
    if (pk_lo == "displaymode")
    {
        if (val == "Minimal") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewDisplayMode::Minimal";
        if (val == "Expanded") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewDisplayMode::Expanded";
        if (val == "Compact") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewDisplayMode::Compact";
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewDisplayMode::Auto";
    }
    // SplitViewDisplayMode enum
    if (pk_lo == "displaymode" && val == "Overlay")
        return "winrt::Microsoft::UI::Xaml::Controls::SplitViewDisplayMode::Overlay";
    if (pk_lo == "displaymode" && val == "Inline")
        return "winrt::Microsoft::UI::Xaml::Controls::SplitViewDisplayMode::Inline";
    // TabWidthMode enum
    if (pk_lo == "tabwidthmode")
    {
        if (val == "Equal") return "winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode::Equal";
        if (val == "SizeToContent") return "winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode::SizeToContent";
        if (val == "Compact") return "winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode::Compact";
        if (val == "CompactEquivalent") return "winrt::Microsoft::UI::Xaml::Controls::TabViewWidthMode::CompactEquivalent";
    }
    // HighContrastAdjustment enum
    if (pk_lo == "highcontrastadjustment")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::ListViewItemHighContrastAdjustment::None";
        if (val == "ApplicationDefault") return "winrt::Microsoft::UI::Xaml::Controls::ListViewItemHighContrastAdjustment::ApplicationDefault";
    }
    // Viewbox StretchDirection enum
    if (pk_lo == "stretchdirection")
    {
        if (val == "UpOnly") return "winrt::Microsoft::UI::Xaml::Controls::StretchDirection::UpOnly";
        if (val == "DownOnly") return "winrt::Microsoft::UI::Xaml::Controls::StretchDirection::DownOnly";
        if (val == "Both") return "winrt::Microsoft::UI::Xaml::Controls::StretchDirection::Both";
    }
    // SelectionFollowsFocus enum (NavigationView.SelectionFollowsFocus)
    if (pk_lo == "selectionfollowsfocus")
    {
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionFollowsFocus::Disabled";
        if (val == "Enabled") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionFollowsFocus::Enabled";
    }
    // Placement (ToolTip) enum
    if (pk_lo == "placement" && (val == "Top" || val == "Bottom" || val == "Left" || val == "Right" || val == "Mouse"))
    {
        return "winrt::Microsoft::UI::Xaml::Controls::Primitives::PlacementMode::" + val;
    }
    // OffContent/OnContent bool -> box_value
    if (pk_lo == "offcontent" || pk_lo == "oncontent")
    {
        if (val == "true" || val == "True") return "winrt::box_value(true)";
        if (val == "false" || val == "False") return "winrt::box_value(false)";
    }
    // PlaceholderText: strip trailing () if present
    if (pk_lo == "placeholdertext")
    {
        std::string clean = val;
        while (clean.size() >= 2 && clean.back() == ')' && clean[clean.size()-2] == '(') { clean.pop_back(); clean.pop_back(); }
        return "L\"" + clean + "\"";
    }

    // Padding/BorderThickness/Margin: all need Thickness wrapping
    if (pk_lo == "padding" || pk_lo == "margin" || pk_lo == "borderthickness")
    {
        // Check if val contains comma (multi-value thickness)
        auto comma = val.find(',');
        if (comma != std::string::npos)
        {
            std::string left = val.substr(0, comma);
            std::string top = val.substr(comma + 1);
            // Trim whitespace
            while (!top.empty() && (top.back() == ' ' || top.back() == '\r' || top.back() == '\n')) top.pop_back();
            while (!left.empty() && (left.front() == ' ' || left.front() == '\r' || left.front() == '\n')) left.erase(0, 1);
            // Check for 4-value: "16,0,16,0"
            auto comma2 = top.find(',');
            if (comma2 != std::string::npos)
            {
                std::string right = top.substr(comma2 + 1);
                top = top.substr(0, comma2);
                auto comma3 = right.find(',');
                if (comma3 != std::string::npos)
                {
                    std::string bottom = right.substr(comma3 + 1);
                    right = right.substr(0, comma3);
                    while (!bottom.empty() && (bottom.back() == ' ' || bottom.back() == '\r' || bottom.back() == '\n')) bottom.pop_back();
                    return "winrt::Microsoft::UI::Xaml::Thickness{" + left + "," + top + "," + right + "," + bottom + "}";
                }
                while (!right.empty() && (right.back() == ' ' || right.back() == '\r' || right.back() == '\n')) right.pop_back();
                return "winrt::Microsoft::UI::Xaml::Thickness{" + left + "," + top + "," + right + "," + right + "}";
            }
            while (!top.empty() && (top.back() == ' ' || top.back() == '\r' || top.back() == '\n')) top.pop_back();
            while (!left.empty() && (left.back() == ' ' || left.back() == '\r' || left.back() == '\n')) left.pop_back();
            // 2-value: left,top -> left,top,right=left,bottom=top
            return "winrt::Microsoft::UI::Xaml::Thickness{" + left + "," + top + "," + left + "," + top + "}";
        }
        else
        {
            // 1-value: uniform thickness OR resource key
            std::string v = val;
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
            while (!v.empty() && (v.front() == ' ' || v.front() == '\r' || v.front() == '\n')) v.erase(0, 1);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            if (v == "PagePadding") return "winrt::Microsoft::UI::Xaml::Thickness{40, 24, 40, 40}";
            if (v == "CardPadding") return "winrt::Microsoft::UI::Xaml::Thickness{20, 20, 20, 20}";
            if (v == "InnerCardPadding") return "winrt::Microsoft::UI::Xaml::Thickness{16, 16, 16, 16}";
            bool is_numeric = !v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-');
            if (is_numeric)
                return "winrt::Microsoft::UI::Xaml::Thickness{" + v + "," + v + "," + v + "," + v + "}";
            return "theme_thickness_th(L\"" + v + "\", winrt::Microsoft::UI::Xaml::Thickness{0,0,0,0})";
        }
    }
    // RenderTransformOrigin: ".5,.5" -> Point{0.5f, 0.5f}
    {
        std::string pk_lo;
        for (char c : pk) pk_lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (pk_lo == "rendertransformorigin")
        {
            auto comma = val.find(',');
            if (comma != std::string::npos)
            {
                std::string px = val.substr(0, comma);
                std::string py = val.substr(comma + 1);
                while (!px.empty() && (px.front() == ' ' || px.front() == '"')) px.erase(0, 1);
                while (!px.empty() && (px.back() == ' ' || px.back() == '"')) px.pop_back();
                while (!py.empty() && (py.front() == ' ' || py.front() == '"')) py.erase(0, 1);
                while (!py.empty() && (py.back() == ' ' || py.back() == '"')) py.pop_back();
                if (!px.empty() && px.front() == '.') px = "0" + px;
                if (!py.empty() && py.front() == '.') py = "0" + py;
                return "winrt::Windows::Foundation::Point{ static_cast<float>(" + px + "f), static_cast<float>(" + py + "f) }";
            }
        }
    }
    // CornerRadius: "8" -> CornerRadius(8,8,8,8) — CornerRadius has no single-value ctor,
    // so always emit all 4 fields. Use fully-qualified type to avoid name collision
    // with the CornerRadius property method on the same struct.
    {
        std::string pk_lo;
        for (char c : pk) pk_lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (pk_lo == "cornerradius")
        {
            auto comma = val.find(',');
            if (comma != std::string::npos)
            {
                std::vector<std::string> parts;
                std::string rest = val;
                while (true)
                {
                    auto c = rest.find(',');
                    if (c == std::string::npos) { parts.push_back(rest); break; }
                    parts.push_back(rest.substr(0, c));
                    rest = rest.substr(c + 1);
                }
                while (parts.size() < 4) parts.push_back(parts.back());
                return "winrt::Microsoft::UI::Xaml::CornerRadius{" + parts[0] + "," + parts[1] + "," + parts[2] + "," + parts[3] + "}";
            }
            std::string v = val;
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
            // CornerRadius has 4 fields: TopLeft, TopRight, BottomRight, BottomLeft
            // Single value means uniform in all corners
            return "winrt::Microsoft::UI::Xaml::CornerRadius{" + v + "," + v + "," + v + "," + v + "}";
        }
    }
    // PathIcon.Data / Path.Data: SVG path data -> real PathGeometry via the
    // parse_svg_path() helper emitted into every view module preamble (this is
    // exactly what XAML's Data type converter does at parse time; Geometry is
    // abstract so a placeholder object cannot be constructed).
    {
        std::string pk_lo_d;
        for (char c : pk) pk_lo_d += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (pk_lo_d == "data" && val.size() > 2
            && (val.find('M') != std::string::npos || val.find('m') != std::string::npos
                || val.find('L') != std::string::npos || val.find('C') != std::string::npos))
        {
            // Path-data grammar contains no quotes/backslashes — safe to embed raw.
            return "parse_svg_path(\"" + val + "\")";
        }
        if (pk_lo_d == "data")
        {
            // Data with a non-path string value: no safe representation — skip.
            return "";
        }
    }
    // Foreground/Background/Fill/Stroke/SelectionHighlightColor: Brush types
    if (pk_lo == "foreground" || pk_lo == "background" || pk_lo == "fill" || pk_lo == "stroke" || pk_lo == "selectionhighlightcolor")
    {
        // If value is a theme resource or empty, return empty — caller skips
        if (val.empty() || val.find('{') != std::string::npos) return "";
        // Already a C++ expression (contains parens or ::) — return as-is
        if (val.find('(') != std::string::npos || val.find("::") != std::string::npos) return "";
        // Named color: all-alpha value -> use make_solid_brush(named_color(...))
        bool all_alpha = !val.empty();
        for (char c : val) { if (!std::isalpha(static_cast<unsigned char>(c))) { all_alpha = false; break; } }
        if (all_alpha && val[0] != '$')
            return "make_solid_brush(named_color(\"" + val + "\"))";
        // Plain identifier (state var) -> wrap in make_solid_brush(named_color(...))
        bool is_ident = !val.empty() && std::isalpha(static_cast<unsigned char>(val[0]));
        for (char c : val) { if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') { is_ident = false; break; } }
        if (is_ident)
            return "make_solid_brush(named_color(\"" + val + "\"))";
        // Otherwise return empty to let caller handle it
        return "";
    }
    // SelectionMode enum
    if (pk_lo == "selectionmode")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::ListViewSelectionMode::None";
        if (val == "Single") return "winrt::Microsoft::UI::Xaml::Controls::ListViewSelectionMode::Single";
        if (val == "Multiple") return "winrt::Microsoft::UI::Xaml::Controls::ListViewSelectionMode::Multiple";
        if (val == "Extended") return "winrt::Microsoft::UI::Xaml::Controls::ListViewSelectionMode::Extended";
    }

    // TreeViewSelectionMode enum (for TreeView.SelectionMode)
    if (pk_lo == "selectionmode")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::TreeViewSelectionMode::None";
        if (val == "Single") return "winrt::Microsoft::UI::Xaml::Controls::TreeViewSelectionMode::Single";
        if (val == "Multiple") return "winrt::Microsoft::UI::Xaml::Controls::TreeViewSelectionMode::Multiple";
        if (val == "Extended") return "winrt::Microsoft::UI::Xaml::Controls::TreeViewSelectionMode::Extended";
    }
    // TextTrimming enum
    if (pk_lo == "texttrimming")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::TextTrimming::None";
        if (val == "Clip") return "winrt::Microsoft::UI::Xaml::TextTrimming::Clip";
        if (val == "CharacterEllipsis") return "winrt::Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis";
        if (val == "WordEllipsis") return "winrt::Microsoft::UI::Xaml::TextTrimming::WordEllipsis";
    }
    // TextAlignment enum
    if (pk_lo == "textalignment")
    {
        if (val == "Left") return "winrt::Microsoft::UI::Xaml::TextAlignment::Left";
        if (val == "Center") return "winrt::Microsoft::UI::Xaml::TextAlignment::Center";
        if (val == "Right") return "winrt::Microsoft::UI::Xaml::TextAlignment::Right";
        if (val == "Justify") return "winrt::Microsoft::UI::Xaml::TextAlignment::Justify";
    }
    // Stretch enum
    {
        std::string pk_lo;
        for (char c : pk) pk_lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (pk_lo == "stretch")
        {
            if (val == "None") return "winrt::Microsoft::UI::Xaml::Media::Stretch::None";
            if (val == "Fill") return "winrt::Microsoft::UI::Xaml::Media::Stretch::Fill";
            if (val == "Uniform") return "winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform";
            if (val == "UniformToFill") return "winrt::Microsoft::UI::Xaml::Media::Stretch::UniformToFill";
        }
    }
    // Colors::Transparent and other color literals
    {
        std::string pk_lo;
        for (char c : pk) pk_lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (pk_lo == "background" || pk_lo == "foreground" || pk_lo == "fill" || pk_lo == "stroke")
        {
            if (val == "Transparent")
                // Transparent is default — skip the property entirely
                return "";
        }
    }
    // FontFamily: its ONLY ctor is FontFamily(String familyName) (see
    // dxaml idl: [method_name("CreateInstanceWithName")] FontFamily(String)).
    // The factory rejects a null familyName with E_INVALIDARG, so the value
    // must be forwarded - never default-construct (that used to crash the
    // app inside CreateInstanceWithName).
    if (pk_lo == "fontfamily")
    {
        std::string clean = strip_wstr(val);
        // strip trailing () artifact (e.g. "Consolas...()" from chained transforms)
        while (clean.size() >= 2 && clean.back() == ')' && clean[clean.size()-2] == '(')
            clean.pop_back(), clean.pop_back();
        if (!clean.empty())
            return "winrt::Microsoft::UI::Xaml::Media::FontFamily(" + wstr_literal(clean) + ")";
        return ""; // nothing usable - skip the setter rather than crash
    }
    // Visibility enum
    if (pk_lo == "visibility" || pk_lo == "visible")
    {
        if (val == "Visible") return "winrt::Microsoft::UI::Xaml::Visibility::Visible";
        if (val == "Collapsed") return "winrt::Microsoft::UI::Xaml::Visibility::Collapsed";
    }
    // IsChecked: bool → IInspectable box_value (projection takes IInspectable)
    if (pk_lo == "ischecked")
    {
        if (val == "true" || val == "True") return "winrt::box_value(true)";
        if (val == "false" || val == "False") return "winrt::box_value(false)";
    }
    // PlaceholderText: hstring, not enum — skip
    if (pk_lo == "placeholdertext")
        return "";
    // Description: hstring, not enum — skip
    if (pk_lo == "description")
        return "";
    // ScrollMode enum (duplicate, use pk_lo)
    if (pk_lo == "horizontalscrollmode" || pk_lo == "verticalscrollmode")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Auto";
        if (val == "Enabled") return "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Enabled";
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Disabled";
    }
    // ScrollBarVisibility enum (duplicate, use pk_lo)
    if (pk_lo == "horizontalscrollbarvisibility" || pk_lo == "verticalscrollbarvisibility")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Auto";
        if (val == "Visible") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Visible";
        if (val == "Hidden") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Hidden";
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled";
    }
    // InfoBarSeverity enum (duplicate, use pk_lo)
    if (pk_lo == "severity")
    {
        std::string lo_val2 = val;
        for (auto& c : lo_val2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_val2 == "informational" || lo_val2 == "info") return "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational";
        if (lo_val2 == "success") return "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success";
        if (lo_val2 == "warning") return "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning";
        if (lo_val2 == "error") return "winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error";
    }
    // FlyoutPlacementMode enum
    if (pk_lo == "placement")
    {
        if (val == "Top") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top";
        if (val == "Bottom") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Bottom";
        if (val == "Left") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Left";
        if (val == "Right") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Right";
        if (val == "Full") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Full";
        if (val == "TopEdgeAlignedLeft") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::TopEdgeAlignedLeft";
        if (val == "TopEdgeAlignedRight") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::TopEdgeAlignedRight";
        if (val == "BottomEdgeAlignedLeft") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedLeft";
        if (val == "BottomEdgeAlignedRight") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::BottomEdgeAlignedRight";
        if (val == "LeftEdgeAlignedTop") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::LeftEdgeAlignedTop";
        if (val == "LeftEdgeAlignedBottom") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::LeftEdgeAlignedBottom";
        if (val == "RightEdgeAlignedTop") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::RightEdgeAlignedTop";
        if (val == "RightEdgeAlignedBottom") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::RightEdgeAlignedBottom";
    }
    // ElementSoundMode enum (duplicate, use pk_lo)
    if (pk_lo == "elementsoundmode")
    {
        if (val == "Default") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::Default";
        if (val == "FocusOnly") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::FocusOnly";
        if (val == "ProgramOnly") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::ProgramOnly";
        if (val == "SystemOnly") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::SystemOnly";
        if (val == "Off") return "winrt::Microsoft::UI::Xaml::ElementSoundMode::Off";
    }
    // SwipeMode enum
    if (pk_lo == "mode")
    {
        if (val == "Reveal") return "winrt::Microsoft::UI::Xaml::Controls::SwipeMode::Reveal";
        if (val == "Execute") return "winrt::Microsoft::UI::Xaml::Controls::SwipeMode::Execute";
    }
    // Slider SnapsTo enum (duplicate, use pk_lo)
    if (pk_lo == "snapsto")
    {
        if (val == "Ticks") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::Ticks";
        if (val == "Value") return "winrt::Microsoft::UI::Xaml::Controls::Primitives::SliderSnapsTo::Value";
    }
    // TickPlacement enum (duplicate, use pk_lo)
    if (pk_lo == "tickplacement")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::TickPlacement::None";
        if (val == "TopLeft") return "winrt::Microsoft::UI::Xaml::Controls::TickPlacement::TopLeft";
        if (val == "BottomRight") return "winrt::Microsoft::UI::Xaml::Controls::TickPlacement::BottomRight";
        if (val == "Outside") return "winrt::Microsoft::UI::Xaml::Controls::TickPlacement::Outside";
        if (val == "Inline") return "winrt::Microsoft::UI::Xaml::Controls::TickPlacement::Inline";
    }
    // TextLineBounds enum
    if (pk_lo == "textlinebounds")
    {
        if (val == "Full") return "winrt::Microsoft::UI::Xaml::TextLineBounds::Full";
        if (val == "Height") return "winrt::Microsoft::UI::Xaml::TextLineBounds::Height";
        if (val == "Tight") return "winrt::Microsoft::UI::Xaml::TextLineBounds::Tight";
        if (val == "TrimToCap") return "winrt::Microsoft::UI::Xaml::TextLineBounds::TrimToCap";
    }
    // ExpandDirection enum
    if (pk == "expanddirection" || pk == "ExpandDirection")
    {
        if (val == "Down") return "winrt::Microsoft::UI::Xaml::Controls::ExpandDirection::Down";
        if (val == "Up") return "winrt::Microsoft::UI::Xaml::Controls::ExpandDirection::Up";
    }
    // NavigationViewPaneDisplayMode enum
    if (pk == "panedisplaymode" || pk == "PaneDisplayMode")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::Auto";
        if (val == "Left") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::Left";
        if (val == "Top") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::Top";
        if (val == "LeftCompact") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::LeftCompact";
        if (val == "LeftMinimal") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::LeftMinimal";
    }
    // ColorPicker ColorSpectrumShape enum
    if (pk == "colorspectrumshape" || pk == "ColorSpectrumShape")
    {
        if (val == "Box") return "winrt::Microsoft::UI::Xaml::Controls::ColorSpectrumShape::Box";
        if (val == "Ring") return "winrt::Microsoft::UI::Xaml::Controls::ColorSpectrumShape::Ring";
    }
    // CalendarSelectionMode enum
    if (pk == "selectionmode" || pk == "SelectionMode")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::CalendarSelectionMode::None";
        if (val == "Single") return "winrt::Microsoft::UI::Xaml::Controls::CalendarSelectionMode::Single";
        if (val == "Multiple") return "winrt::Microsoft::UI::Xaml::Controls::CalendarSelectionMode::Multiple";
        if (val == "Range") return "winrt::Microsoft::UI::Xaml::Controls::CalendarSelectionMode::Range";
    }
    // SwipeItem BehaviorOnInvoked enum
    if (pk == "behavioroninvoked" || pk == "BehaviorOnInvoked")
    {
        if (val == "Dismiss") return "winrt::Microsoft::UI::Xaml::Controls::SwipeBehaviorOnInvoked::Dismiss";
        if (val == "DismissAndReveal") return "winrt::Microsoft::UI::Xaml::Controls::SwipeBehaviorOnInvoked::DismissAndReveal";
        if (val == "StayOpenOnReveal") return "winrt::Microsoft::UI::Xaml::Controls::SwipeBehaviorOnInvoked::StayOpenOnReveal";
    }
    // PasswordRevealMode enum
    if (pk == "passwordrevealmode" || pk == "PasswordRevealMode")
    {
        if (val == "Hidden") return "winrt::Microsoft::UI::Xaml::Controls::PasswordRevealMode::Hidden";
        if (val == "Visible") return "winrt::Microsoft::UI::Xaml::Controls::PasswordRevealMode::Visible";
        if (val == "Peek") return "winrt::Microsoft::UI::Xaml::Controls::PasswordRevealMode::Peek";
    }
    // KeyboardAcceleratorPlacementMode enum
    if (pk == "keyboardacceleratorplacementmode" || pk == "KeyboardAcceleratorPlacementMode")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorPlacementMode::Auto";
        if (val == "Hidden") return "winrt::Microsoft::UI::Xaml::Input::KeyboardAcceleratorPlacementMode::Hidden";
    }
    // NavigationViewSelectionFollowsFocus enum
    if (pk == "selectionfollowsfocus" || pk == "SelectionFollowsFocus")
    {
        if (val == "Never") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionFollowsFocus::Never";
        if (val == "Always") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionFollowsFocus::Always";
        if (val == "Programmatically") return "winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionFollowsFocus::Programmatically";
    }
    // CommandBar DefaultLabelPosition enum
    if (pk == "defaultlabelposition" || pk == "DefaultLabelPosition")
    {
        if (val == "Bottom") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Bottom";
        if (val == "Right") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Right";
        if (val == "Collapsed") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Collapsed";
        if (val == "Top") return "winrt::Microsoft::UI::Xaml::Controls::CommandBarDefaultLabelPosition::Top";
    }
    // VirtualKeyModifiers for KeyboardAccelerator.Modifiers (flags enum)
    if (pk == "modifiers" || pk == "Modifiers")
    {
        if (val == "Control") return "winrt::Windows::System::VirtualKeyModifiers::Control";
        if (val == "Shift") return "winrt::Windows::System::VirtualKeyModifiers::Shift";
        if (val == "Alt" || val == "Menu") return "winrt::Windows::System::VirtualKeyModifiers::Menu";
        if (val == "Windows") return "winrt::Windows::System::VirtualKeyModifiers::Windows";
    }
    // SplitView DisplayMode enum
    if (pk == "displaymode" || pk == "DisplayMode")
    {
        if (val == "Overlay") return "winrt::Microsoft::UI::Xaml::Controls::SplitViewDisplayMode::Overlay";
        if (val == "Inline") return "winrt::Microsoft::UI::Xaml::Controls::SplitViewDisplayMode::Inline";
        if (val == "CompactOverlay") return "winrt::Microsoft::UI::Xaml::Controls::SplitViewDisplayMode::CompactOverlay";
        if (val == "CompactInline") return "winrt::Microsoft::UI::Xaml::Controls::SplitViewDisplayMode::CompactInline";
    }
    // LinedFlowLayout ItemsStretch enum
    if (pk == "itemsstretch" || pk == "ItemsStretch")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::LinedFlowLayoutItemsStretch::None";
        if (val == "Fill") return "winrt::Microsoft::UI::Xaml::Controls::LinedFlowLayoutItemsStretch::Fill";
    }
    // ItemsView TabFocusNavigation / SelectionMode
    if (pk == "tabfocusnavigation" || pk == "TabFocusNavigation")
    {
        if (val == "Local") return "winrt::Microsoft::UI::Xaml::Input::KeyboardNavigationMode::Local";
        if (val == "Cycle") return "winrt::Microsoft::UI::Xaml::Input::KeyboardNavigationMode::Cycle";
        if (val == "Once") return "winrt::Microsoft::UI::Xaml::Input::KeyboardNavigationMode::Once";
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Input::KeyboardNavigationMode::Disabled";
    }
    // ScrollView ContentOrientation enum
    if (pk == "contentorientation" || pk == "ContentOrientation")
    {
        if (val == "Vertical") return "winrt::Microsoft::UI::Xaml::Controls::ScrollContentOrientation::Vertical";
        if (val == "Horizontal") return "winrt::Microsoft::UI::Xaml::Controls::ScrollContentOrientation::Horizontal";
        if (val == "Both") return "winrt::Microsoft::UI::Xaml::Controls::ScrollContentOrientation::Both";
    }
    // ZoomMode enum
    if (pk == "zoommode" || pk == "ZoomMode")
    {
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Disabled";
        if (val == "Enabled") return "winrt::Microsoft::UI::Xaml::Controls::ZoomMode::Enabled";
    }
    // Viewbox StretchDirection enum
    if (pk == "stretchdirection" || pk == "StretchDirection")
    {
        if (val == "UpOnly") return "winrt::Microsoft::UI::Xaml::Controls::StretchDirection::UpOnly";
        if (val == "DownOnly") return "winrt::Microsoft::UI::Xaml::Controls::StretchDirection::DownOnly";
        if (val == "Both") return "winrt::Microsoft::UI::Xaml::Controls::StretchDirection::Both";
    }
    // GeometryGroup FillRule enum
    if (pk == "fillrule" || pk == "FillRule")
    {
        if (val == "EvenOdd") return "winrt::Microsoft::UI::Xaml::Media::FillRule::EvenOdd";
        if (val == "Nonzero") return "winrt::Microsoft::UI::Xaml::Media::FillRule::Nonzero";
    }
    // HighContrastAdjustment enum
    if (pk == "highcontrastadjustment" || pk == "HighContrastAdjustment")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Controls::HighContrastAdjustment::None";
        if (val == "Application") return "winrt::Microsoft::UI::Xaml::Controls::HighContrastAdjustment::Application";
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Controls::HighContrastAdjustment::Auto";
    }
    // TextAlignment enum (additional)
    if (pk == "textalignment" || pk == "TextAlignment")
    {
        if (val == "DetectFromContent") return "winrt::Microsoft::UI::Xaml::TextAlignment::DetectFromContent";
    }
    // XYFocusKeyboardNavigationMode enum
    if (pk == "xyfocuskeyboardnavigation" || pk == "XYFocusKeyboardNavigation")
    {
        if (val == "Enabled") return "winrt::Microsoft::UI::Xaml::Input::XYFocusKeyboardNavigationMode::Enabled";
        if (val == "Disabled") return "winrt::Microsoft::UI::Xaml::Input::XYFocusKeyboardNavigationMode::Disabled";
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Input::XYFocusKeyboardNavigationMode::Auto";
    }
    // XYFocusNavigationStrategy enum
    if (pk == "xyfocusnavigationstrategy" || pk == "XYFocusNavigationStrategy")
    {
        if (val == "Auto") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategy::Auto";
        if (val == "KeyNavigation") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategy::KeyNavigation";
        if (val == "DirectionNavigation") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategy::DirectionNavigation";
        if (val == "Menu") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategy::Menu";
        if (val == "Legacy") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategy::Legacy";
    }
    // XYFocusNavigationStrategyOverride
    if (pk == "xyfocusnavigationstrategyoverride" || pk == "XYFocusNavigationStrategyOverride")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategyOverride::None";
        if (val == "KeyNavigation") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategyOverride::KeyNavigation";
        if (val == "DirectionNavigation") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategyOverride::DirectionNavigation";
        if (val == "Menu") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategyOverride::Menu";
        if (val == "Legacy") return "winrt::Microsoft::UI::Xaml::Input::XYFocusNavigationStrategyOverride::Legacy";
    }
    // XYFocusKeyNavigation enum
    if (pk == "xyfocuskeynavigation" || pk == "XYFocusKeyNavigation")
    {
        if (val == "None") return "winrt::Microsoft::UI::Xaml::Input::XYFocusKeyNavigation::None";
        if (val == "Key0") return "winrt::Microsoft::UI::Xaml::Input::XYFocusKeyNavigation::Key0";
        if (val == "Key1") return "winrt::Microsoft::UI::Xaml::Input::XYFocusKeyNavigation::Key1";
    }
    // ExpandDirection enum
    if (pk == "expanddirection" || pk == "ExpandDirection")
    {
        if (val == "Down") return "winrt::Microsoft::UI::Xaml::Controls::ExpandDirection::Down";
        if (val == "Up") return "winrt::Microsoft::UI::Xaml::Controls::ExpandDirection::Up";
    }
    // ColorPickerColorSpectrumComponents enum
    if (pk == "colorspectrumcomponents" || pk == "ColorSpectrumComponents")
    {
        if (val == "Hue") return "winrt::Microsoft::UI::Xaml::Controls::ColorSpectrumComponents::HueSaturation";
        if (val == "HueSaturation") return "winrt::Microsoft::UI::Xaml::Controls::ColorSpectrumComponents::HueSaturation";
        if (val == "HueValue") return "winrt::Microsoft::UI::Xaml::Controls::ColorSpectrumComponents::HueValue";
        if (val == "SaturationValue") return "winrt::Microsoft::UI::Xaml::Controls::ColorSpectrumComponents::SaturationValue";
    }
    // NavigationViewPaneToggleEvent (not projected — just emit as no-op)
    // ProgressBarStatus enum
    if (pk == "status" || pk == "Status")
    {
        if (val == "Error") return "winrt::Microsoft::UI::Xaml::Controls::ProgressBarStatus::Error";
        if (val == "Indeterminate") return "winrt::Microsoft::UI::Xaml::Controls::ProgressBarStatus::Indeterminate";
        if (val == "Paused") return "winrt::Microsoft::UI::Xaml::Controls::ProgressBarStatus::Paused";
        if (val == "Running") return "winrt::Microsoft::UI::Xaml::Controls::ProgressBarStatus::Running";
    }
    // ItemsSource: IInspectable box_value wrapper for string state vars
    if (pk == "itemssource" || pk == "ItemsSource")
    {
        if (!val.empty() && val != "nullptr")
        {
            bool is_num = std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-';
            if (is_num) return "winrt::box_value(" + val + ")";
            return "winrt::box_value(L\"" + val + "\")";
        }
    }
    // Tag/SelectedItem: IInspectable box_value wrapper
    if (pk == "tag" || pk == "Tag" || pk == "selecteditem" || pk == "SelectedItem")
    {
        if (!val.empty() && val != "nullptr")
        {
            bool is_num = std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-';
            if (is_num) return "winrt::box_value(" + val + ")";
            return "winrt::box_value(L\"" + val + "\")";
        }
    }
    // IconSource: construct proper IconSource type from string value
    if (pk == "iconsource" || pk == "IconSource")
    {
        if (val.empty()) return "";
        // Already a C++ expression (contains :: or () or {)
        if (val.find("::") != std::string::npos || val.find("(") != std::string::npos || val.find("{") != std::string::npos)
            return val;
        // Strip L"..." or "..." wrapping
        std::string clean = val;
        if (clean.size() > 2 && clean[0] == 'L' && clean[1] == '"')
            clean = clean.substr(2, clean.size() - 3);
        else if (clean.size() >= 2 && clean.front() == '"' && clean.back() == '"')
            clean = clean.substr(1, clean.size() - 2);
        // Map type name strings to proper IconSource construction
        if (clean == "SymbolIcon" || clean == "symbolicon")
            return "SymbolIconSource()";
        if (clean == "FontIcon" || clean == "fonticon")
            return "FontIconSource()";
        if (clean == "BitmapIcon" || clean == "bitmapicon")
            return "BitmapIconSource()";
        if (clean == "PathIcon" || clean == "pathicon")
            return "PathIconSource()";
        if (clean == "ImageIcon" || clean == "imageicon")
            return "ImageIconSource()";
        // If it looks like a symbol name (PascalCase, not a type), emit SymbolIconSource with glyph
        if (!clean.empty() && std::isupper(static_cast<unsigned char>(clean[0])))
            return "SymbolIconSource(Symbol::" + clean + ")";
        return ""; // caller skips empty
    }
    // ContainerContentChanging, ChoosingItemContainer: event handlers — not properties
    {
        std::string lo_pk2(pk);
        for (auto& c : lo_pk2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo_pk2 == "containercontentchanging" || lo_pk2 == "choosingitemcontainer"
            || lo_pk2 == "choosingsemanticzoomstate")
            return ""; // skip — event handler, not property
    }
    return {}; // no special handling
}

// Normalize image source paths: ensure ms-appx:/// prefix and forward slashes
inline std::string normalize_image_path(std::string const& raw)
{
    std::string s = raw;
    for (auto& c : s) if (c == '\\') c = '/';
    if (!s.empty() && s[0] == '/' && s.size() > 1 && s[1] == '/')
        s = "ms-appx:" + s;
    else if (!s.empty() && s[0] == '/')
        s = "ms-appx://" + s;
    else if (s.rfind("Assets/", 0) == 0)
        s = "ms-appx:///" + s;
    return s;
}

// Resolve icon-related property values (Icon, IconSource) to proper construction.
// Returns empty string if the property/value pair doesn't need special handling.
// Handles: SymbolIcon(Symbol::Xxx), FontIcon, ImageIcon, PathIcon, BitmapIcon, AnimatedIcon.
inline std::string resolve_icon_value(std::string_view pk, std::string const& val)
{
    if (val.empty()) return "";
    std::string lo_pk;
    lo_pk.reserve(pk.size());
    for (auto c : pk) lo_pk += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool is_icon_prop = (lo_pk == "icon" || lo_pk == "iconsource" || lo_pk == "queryicon");
    if (!is_icon_prop) return "";
    // Already a C++ expression
    if (val.find("::") != std::string::npos || val.find("(") != std::string::npos) return "";
    // Strip L"..." or "..." wrapping
    std::string clean = val;
    if (clean.size() > 2 && clean[0] == 'L' && clean[1] == '"')
        clean = clean.substr(2, clean.size() - 3);
    else if (clean.size() >= 2 && clean.front() == '"' && clean.back() == '"')
        clean = clean.substr(1, clean.size() - 2);
    if (clean.empty()) return "";
    // Type names that need construction wrapping
    if (clean == "SymbolIcon" || clean == "symbolicon") return "SymbolIcon(symbol_from_name(L\"None\"))";
    if (clean == "FontIcon" || clean == "fonticon") return "FontIcon()";
    if (clean == "BitmapIcon" || clean == "bitmapicon") return "BitmapIcon()";
    if (clean == "PathIcon" || clean == "pathicon") return "PathIcon()";
    if (clean == "ImageIcon" || clean == "imageicon") return "ImageIcon()";
    if (clean == "AnimatedIcon" || clean == "animatedicon") return "AnimatedIcon()";
    // If it's a PascalCase symbol name, use symbol_from_name (Symbol:: enum not projected)
    if (std::isupper(static_cast<unsigned char>(clean[0])))
        return "SymbolIcon(symbol_from_name(L\"" + clean + "\"))";
    return "";
}

} // namespace winuic
