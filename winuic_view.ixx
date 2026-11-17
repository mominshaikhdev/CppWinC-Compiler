// winuic_view.ixx — View module generation and code fixup post-pass
// Part of module winuic_data
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

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
    // Include metadata-backed object fields so later expression rewrites know
    // the projected type of x:Name and property-element state members.
    for (auto const& ln : lines)
    {
        size_t p = ln.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        size_t type_end = ln.find_first_of(" \t", p);
        if (type_end == std::string::npos) continue;
        std::string type = ln.substr(p, type_end - p);
        size_t scope = type.rfind("::");
        std::string type_name = scope == std::string::npos ? type : type.substr(scope + 2);
        std::string type_key = type_name;
        for (auto& ch : type_key)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        bool projected_object = type.find("::Controls::") != std::string::npos
            || (prop_kinds && prop_kinds->count(type_key));
        if (!projected_object) continue;
        size_t name_begin = ln.find_first_not_of(" \t", type_end);
        if (name_begin == std::string::npos) continue;
        size_t name_end = name_begin;
        while (name_end < ln.size()
            && (std::isalnum(static_cast<unsigned char>(ln[name_end])) || ln[name_end] == '_'))
            ++name_end;
        if (name_end == name_begin) continue;
        size_t terminator = ln.find_first_not_of(" \t", name_end);
        if (terminator == std::string::npos || (ln[terminator] != ';' && ln[terminator] != '=')) continue;
        vtype[ln.substr(name_begin, name_end - name_begin)] = std::move(type_name);
    }

    auto rewrite_typed_member_reads = [&](std::string expression)
    {
        if (trim(expression).rfind("//", 0) == 0) return expression;
        auto metadata_key = [](std::string type)
        {
            if (auto generic = type.find('<'); generic != std::string::npos) type.resize(generic);
            if (auto scope = type.find_last_of(".:"); scope != std::string::npos) type = type.substr(scope + 1);
            for (auto& ch : type) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return type;
        };
        bool in_string = false;
        bool escaped = false;
        for (std::size_t p = 0; p < expression.size();)
        {
            char ch = expression[p];
            if (in_string)
            {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') in_string = false;
                ++p;
                continue;
            }
            if (ch == '"') { in_string = true; ++p; continue; }
            if (!(std::isalpha(static_cast<unsigned char>(ch)) || ch == '_')) { ++p; continue; }
            std::size_t end = p + 1;
            while (end < expression.size()
                && (std::isalnum(static_cast<unsigned char>(expression[end])) || expression[end] == '_')) ++end;
            auto variable = vtype.find(expression.substr(p, end - p));
            bool boundary = p == 0
                || !(std::isalnum(static_cast<unsigned char>(expression[p - 1])) || expression[p - 1] == '_');
            bool this_access = p >= 6 && expression.compare(p - 6, 6, "this->") == 0;
            bool chained = (p > 0 && expression[p - 1] == '.')
                || (p >= 2 && expression.compare(p - 2, 2, "->") == 0);
            if (!boundary || (chained && !this_access) || variable == vtype.end()
                || end >= expression.size() || expression[end] != '.')
            {
                p = end;
                continue;
            }
            std::string current_type = variable->second;
            std::size_t dot = end;
            while (dot < expression.size() && expression[dot] == '.')
            {
                std::size_t member = dot + 1, member_end = member;
                while (member_end < expression.size()
                    && (std::isalnum(static_cast<unsigned char>(expression[member_end])) || expression[member_end] == '_'))
                    ++member_end;
                if (member_end == member) break;
                std::string key = expression.substr(member, member_end - member);
                for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!prop_kinds) break;
                auto properties = prop_kinds->find(metadata_key(current_type));
                if (properties == prop_kinds->end()) break;
                auto property = properties->second.find(key);
                if (property == properties->second.end()) break;
                current_type = property->second;
                if (member_end < expression.size() && expression[member_end] == '(')
                {
                    int depth = 1;
                    std::size_t call_end = member_end + 1;
                    for (; call_end < expression.size() && depth; ++call_end)
                    {
                        if (expression[call_end] == '(') ++depth;
                        else if (expression[call_end] == ')') --depth;
                    }
                    dot = call_end;
                }
                else
                {
                    expression.insert(member_end, "()");
                    dot = member_end + 2;
                }
            }
            p = dot > end ? dot : end;
        }
        return expression;
    };

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
        ln = rewrite_typed_member_reads(std::move(ln));
        std::string line_shape = trim(ln);
        if (line_shape.rfind("if (", 0) == 0
            || line_shape.rfind("else if (", 0) == 0
            || line_shape.rfind("else", 0) == 0)
            continue;
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
        {
            std::string key = argT;
            if (key.size() > 2 && key[0] == 'L' && key[1] == '"') key = key.substr(2, key.size() - 3);
            else if (key.size() >= 2 && key.front() == '"' && key.back() == '"') key = key.substr(1, key.size() - 2);
            if (key == "Auto" || key == "NaN") {
                comment = true;
            } else {
                std::string rep = "theme_double_th(L\"" + key + "\", 12.0)";
                auto pos = out_line.rfind(argT);
                if (pos != std::string::npos)
                    out_line = out_line.substr(0, pos) + rep + out_line.substr(pos + argT.size());
                else
                    comment = true;
            }
        }
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
                        {
                            if (std::string(src_props[sp]) == "ItemsSource")
                                out_line = out_line.substr(0, b3) + "winrt::IInspectable{nullptr}" + out_line.substr(e3);
                            else
                                out_line = out_line.substr(0, b3) + "winrt::IInspectable{nullptr}" + out_line.substr(e3);
                        }
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



// Generate winui_base.ixx: the shared base module for WinUI 3 views.
// Emits platform imports, exported runtime helpers, component stubs, and utility converters once.
inline std::string generate_winui_base_module(
    std::string const& ns_meta,
    attribute_catalog const& catalog,
    bool const have_windows_sdk = false,
    std::set<std::string> const& projected_names = {},
    std::vector<std::string> const& extra_platform_ns = {})
{
    (void)catalog;
    std::ostringstream out;
    std::string ns = ident_ns(ns_meta);

    out << "// winui_base.ixx — Base runtime module for WinUI 3 views.\n";
    out << "// generated by cppwinc; shared across all view modules to avoid duplicate prelude parsing.\n\n";
    out << "module;\n\n";
    out << "export module winui_base;\n\n";
    out << "import std;\n";
    out << "export import winrt_base;\n";
    out << "export import winrt.Microsoft.UI.Xaml;\n";
    out << "export import winrt.Microsoft.UI.Xaml.Hosting;\n";
    out << "export import winrt.Microsoft.UI.Dispatching;\n";
    out << "export import winrt.Microsoft.UI.Xaml.Controls;\n";
    out << "export import winrt.Microsoft.UI.Xaml.Controls.Primitives;\n";
    out << "export import winrt.Microsoft.UI.Xaml.Media;\n";
    out << "export import winrt.Microsoft.UI.Xaml.Automation;\n";
    out << "export import winrt.Microsoft.UI.Xaml.Markup;\n";
    out << "export import winrt.Windows.UI.Xaml.Interop;\n";
    out << "export import winrt.Microsoft.UI.Windowing;\n";
    out << "export import Windows.Win32.UI.WindowsAndMessaging;\n";
    for (auto const& xns : extra_platform_ns)
        out << "export import winrt." << xns << ";\n";
    if (have_windows_sdk)
    {
        out << "export import winrt.Windows.Foundation;\n";
        out << "export import winrt.Windows.Foundation.Collections;\n";
        out << "export import winrt.Windows.UI;\n";
        out << "export import winrt.Windows.UI.Text;\n";
    }
    else
    {
        out << "export namespace winrt::Windows::UI {\n";
        out << "struct Color { std::uint8_t A{255}, R{0}, G{0}, B{0}; };\n";
        out << "} // namespace winrt::Windows::UI\n\n";
    }
    if (!ns.empty())
        out << "export import " << ns << ";\n\n";

    // Runtime helpers (exported inline / export extern "C")
    emit_runtime_helpers(out, have_windows_sdk, ns_meta, /*is_shared_module=*/true);

    // Missing component type stubs (exported namespaces)
    emit_component_stubs(out, ns, projected_names, /*is_shared_module=*/true);

    out << "// Projected IVector<T>/IObservableVector<T> wrappers expose get_abi() but don't\n";
    out << "// convert to IInspectable; collection classes (InlineCollection, UIElementCollection)\n";
    out << "// convert to IInspectable but have no get_abi(). Route both to their ABI pointer.\n";
    out << "export template <typename TColl>\n";
    out << "    requires requires(TColl const& c) { c.get_abi(); }\n";
    out << "inline void* collection_abi(TColl const& c) { return c.get_abi(); }\n";
    out << "export inline void* collection_abi(winrt::IInspectable const& c) { return c.get_abi(); }\n";
    out << "export inline void* collection_abi(void* p) { return p; }\n";
    out << "export template <typename TColl, typename TItem>\n";
    out << "    requires requires(TColl& c, TItem const& i) { c.Append(i); }\n";
    out << "inline void inspectable_append(TColl collection, TItem const& item) {\n";
    out << "    collection.Append(item);\n";
    out << "}\n";
    out << "export template <typename T, typename TItem>\n";
    out << "inline void inspectable_append(winrt::Windows::Foundation::Collections::IObservableVector<T> const& collection, TItem const& item) {\n";
    out << "    if constexpr (requires { collection.Append(item); }) {\n";
    out << "        collection.Append(item);\n";
    out << "        return;\n";
    out << "    }\n";
    out << "    void* col_ptr = collection.get_abi();\n";
    out << "    if (!col_ptr) return;\n";
    out << "    void* vec_ptr = nullptr;\n";
    out << "    static winrt::winrt_guid const vec_iid = winrt::Windows::Foundation::Collections::IVector<T>::iid();\n";
    out << "    if (winrt::impl::query_interface(col_ptr, vec_iid, &vec_ptr) != 0 || !vec_ptr) return;\n";
    out << "    void* item_raw = collection_abi(item);\n";
    out << "    void* elem_ptr = nullptr;\n";
    out << "    if constexpr (requires { winrt::iid_of<T>; }) {\n";
    out << "        if (item_raw) {\n";
    out << "            winrt::impl::query_interface(item_raw, winrt::iid_of<T>, &elem_ptr);\n";
    out << "        }\n";
    out << "    }\n";
    out << "    if (!elem_ptr) elem_ptr = item_raw;\n";
    out << "    auto** vtbl = *reinterpret_cast<void***>(vec_ptr);\n";
    out << "    using append_fn = std::int32_t(__stdcall*)(void*, void*);\n";
    out << "    reinterpret_cast<append_fn>(vtbl[13])(vec_ptr, elem_ptr);\n";
    out << "    if (elem_ptr && elem_ptr != item_raw) {\n";
    out << "        auto** elem_vtbl = *reinterpret_cast<void***>(elem_ptr);\n";
    out << "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(elem_vtbl[2])(elem_ptr);\n";
    out << "    }\n";
    out << "    reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vtbl[2])(vec_ptr);\n";
    out << "}\n";
    out << "export template <typename T, typename TItem>\n";
    out << "inline void inspectable_append(winrt::Windows::Foundation::Collections::IVector<T> const& collection, TItem const& item) {\n";
    out << "    if constexpr (requires { collection.Append(item); }) {\n";
    out << "        collection.Append(item);\n";
    out << "        return;\n";
    out << "    }\n";
    out << "    void* col_ptr = collection.get_abi();\n";
    out << "    if (!col_ptr) return;\n";
    out << "    void* item_raw = collection_abi(item);\n";
    out << "    void* elem_ptr = nullptr;\n";
    out << "    if constexpr (requires { winrt::iid_of<T>; }) {\n";
    out << "        if (item_raw) {\n";
    out << "            winrt::impl::query_interface(item_raw, winrt::iid_of<T>, &elem_ptr);\n";
    out << "        }\n";
    out << "    }\n";
    out << "    if (!elem_ptr) elem_ptr = item_raw;\n";
    out << "    auto** vtbl = *reinterpret_cast<void***>(col_ptr);\n";
    out << "    using append_fn = std::int32_t(__stdcall*)(void*, void*);\n";
    out << "    reinterpret_cast<append_fn>(vtbl[13])(col_ptr, elem_ptr);\n";
    out << "    if (elem_ptr && elem_ptr != item_raw) {\n";
    out << "        auto** elem_vtbl = *reinterpret_cast<void***>(elem_ptr);\n";
    out << "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(elem_vtbl[2])(elem_ptr);\n";
    out << "    }\n";
    out << "}\n";
    out << "export template <typename TColl>\n";
    out << "    requires (!requires(TColl& c, winrt::IInspectable const& i) { c.Append(i); })\n";
    out << "inline void inspectable_append(TColl const& collection, winrt::IInspectable const& item) {\n";
    out << "    void* col_ptr = collection_abi(collection);\n";
    out << "    if (!col_ptr) return;\n";
    out << "    void* item_ptr = collection_abi(item);\n";
    out << "    if (!item_ptr) return;\n";
    out << "    static winrt::winrt_guid const ivector_inspectable_iid{ 0x4B52C37C, 0xD426, 0x42E0, { 0xA4, 0x49, 0xF2, 0x6E, 0x14, 0x3F, 0x1A, 0xEC } };\n";
    out << "    void* vec_ptr = nullptr;\n";
    out << "    if (winrt::impl::query_interface(col_ptr, ivector_inspectable_iid, &vec_ptr) == 0 && vec_ptr) {\n";
    out << "        auto** vtbl = *reinterpret_cast<void***>(vec_ptr);\n";
    out << "        using append_fn = std::int32_t(__stdcall*)(void*, void*);\n";
    out << "        reinterpret_cast<append_fn>(vtbl[13])(vec_ptr, item_ptr);\n";
    out << "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vtbl[2])(vec_ptr);\n";
    out << "    } else {\n";
    out << "        auto** vtbl = *reinterpret_cast<void***>(col_ptr);\n";
    out << "        using append_fn = std::int32_t(__stdcall*)(void*, void*);\n";
    out << "        reinterpret_cast<append_fn>(vtbl[13])(col_ptr, item_ptr);\n";
    out << "    }\n";
    out << "}\n\n";

    out << "export inline winrt::Microsoft::UI::Xaml::Visibility make_visibility(bool v) {\n";
    out << "    return v ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;\n";
    out << "}\n";
    out << "export inline winrt::Microsoft::UI::Xaml::Visibility make_visibility(winrt::Microsoft::UI::Xaml::Visibility v) { return v; }\n";
    out << "export inline winrt::Microsoft::UI::Xaml::Visibility make_visibility(winrt::hstring const& v) {\n";
    out << "    auto sv = v.to_view();\n";
    out << "    if (sv == L\"Collapsed\" || sv == L\"collapsed\" || sv == L\"False\" || sv == L\"false\" || sv == L\"0\" || sv.empty())\n";
    out << "        return winrt::Microsoft::UI::Xaml::Visibility::Collapsed;\n";
    out << "    return winrt::Microsoft::UI::Xaml::Visibility::Visible;\n";
    out << "}\n";
    out << "export inline winrt::Microsoft::UI::Xaml::Visibility make_visibility(winrt::IInspectable const& v) {\n";
    out << "    return v.get_abi() != nullptr ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;\n";
    out << "}\n\n";

    out << "export struct formatted_string {\n";
    out << "    winrt::hstring value;\n";
    out << "    formatted_string Replace(winrt::hstring const& from, winrt::hstring const& to) const {\n";
    out << "        std::wstring text(value.c_str());\n";
    out << "        std::wstring needle(from.c_str());\n";
    out << "        std::wstring replacement(to.c_str());\n";
    out << "        if (!needle.empty()) {\n";
    out << "            std::size_t offset = 0;\n";
    out << "            while ((offset = text.find(needle, offset)) != std::wstring::npos) {\n";
    out << "                text.replace(offset, needle.size(), replacement);\n";
    out << "                offset += replacement.size();\n";
    out << "            }\n";
    out << "        }\n";
    out << "        return {winrt::hstring(text.c_str())};\n";
    out << "    }\n";
    out << "    operator winrt::hstring() const { return value; }\n";
    out << "};\n";
    out << "export inline formatted_string format_color(winrt::Windows::UI::Color const& value) {\n";
    out << "    static constexpr wchar_t digits[] = L\"0123456789ABCDEF\";\n";
    out << "    std::uint8_t channels[] = {value.A, value.R, value.G, value.B};\n";
    out << "    wchar_t text[10] = {L'#'};\n";
    out << "    for (std::size_t i = 0; i < 4; ++i) {\n";
    out << "        text[1 + i * 2] = digits[channels[i] >> 4];\n";
    out << "        text[2 + i * 2] = digits[channels[i] & 0x0F];\n";
    out << "    }\n";
    out << "    return {winrt::hstring(text)};\n";
    out << "}\n";
    out << "export inline formatted_string format_color(winrt::IInspectable const& value) {\n";
    out << "    void* reference_raw = nullptr;\n";
    out << "    winrt::check_hresult(winrt::impl::query_interface(value.get_abi(), winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>::iid(), &reference_raw));\n";
    out << "    auto reference = winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>::from_abi(reference_raw);\n";
    out << "    return format_color(reference.get_Value());\n";
    out << "}\n\n";

    out << "export inline winrt::Microsoft::UI::Xaml::Controls::Symbol symbol_from_name(winrt::hstring const& name) {\n";
    out << "    auto sv = name.to_view();\n";
    out << "    if (sv == L\"Favorite\" || sv == L\"Favorites\" || sv == L\"Like\" || sv == L\"Star\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Favorite;\n";
    out << "    if (sv == L\"Accept\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Accept;\n";
    out << "    if (sv == L\"Add\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Add;\n";
    out << "    if (sv == L\"Back\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Back;\n";
    out << "    if (sv == L\"Cancel\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Cancel;\n";
    out << "    if (sv == L\"CheckMark\") return static_cast<winrt::Microsoft::UI::Xaml::Controls::Symbol>(57651);\n";
    out << "    if (sv == L\"Clear\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Clear;\n";
    out << "    if (sv == L\"Close\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::ClosePane;\n";
    out << "    if (sv == L\"Copy\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Copy;\n";
    out << "    if (sv == L\"Delete\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Delete;\n";
    out << "    if (sv == L\"Edit\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Edit;\n";
    out << "    if (sv == L\"Find\" || sv == L\"Search\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Find;\n";
    out << "    if (sv == L\"Forward\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Forward;\n";
    out << "    if (sv == L\"Help\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Help;\n";
    out << "    if (sv == L\"Home\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Home;\n";
    out << "    if (sv == L\"Like\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Favorite;\n";
    out << "    if (sv == L\"Mail\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Mail;\n";
    out << "    if (sv == L\"MediaPlay\" || sv == L\"Play\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Play;\n";
    out << "    if (sv == L\"MediaPause\" || sv == L\"Pause\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Pause;\n";
    out << "    if (sv == L\"MediaStop\" || sv == L\"Stop\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Stop;\n";
    out << "    if (sv == L\"More\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::More;\n";
    out << "    if (sv == L\"None\") return static_cast<winrt::Microsoft::UI::Xaml::Controls::Symbol>(0);\n";
    out << "    if (sv == L\"Redo\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Redo;\n";
    out << "    if (sv == L\"Refresh\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Refresh;\n";
    out << "    if (sv == L\"Save\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Save;\n";
    out << "    if (sv == L\"Send\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Send;\n";
    out << "    if (sv == L\"Setting\" || sv == L\"Settings\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Setting;\n";
    out << "    if (sv == L\"Share\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Share;\n";
    out << "    if (sv == L\"Sort\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Sort;\n";
    out << "    if (sv == L\"Sync\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Sync;\n";
    out << "    if (sv == L\"Undo\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Undo;\n";
    out << "    if (sv == L\"Zoom\" || sv == L\"ZoomIn\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::ZoomIn;\n";
    out << "    if (sv == L\"Clock\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Clock;\n";
    out << "    if (sv == L\"Emoji\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Emoji;\n";
    out << "    if (sv == L\"People\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::People;\n";
    out << "    if (sv == L\"Camera\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Camera;\n";
    out << "    if (sv == L\"Video\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Video;\n";
    out << "    if (sv == L\"Folder\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Folder;\n";
    out << "    if (sv == L\"Audio\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Audio;\n";
    out << "    if (sv == L\"Document\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Document;\n";
    out << "    if (sv == L\"Page\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Page;\n";
    out << "    if (sv == L\"Filter\") return winrt::Microsoft::UI::Xaml::Controls::Symbol::Filter;\n";
    out << "    if (sv == L\"Ringer\") return static_cast<winrt::Microsoft::UI::Xaml::Controls::Symbol>(62242);\n";
    out << "    if (sv == L\"EmojiFrown\") return static_cast<winrt::Microsoft::UI::Xaml::Controls::Symbol>(57655);\n";
    out << "    if (sv == L\"EmojiSmile\") return static_cast<winrt::Microsoft::UI::Xaml::Controls::Symbol>(57656);\n";
    out << "    return static_cast<winrt::Microsoft::UI::Xaml::Controls::Symbol>(0);\n";
    out << "}\n\n";

    out << "export inline winrt::Microsoft::UI::Xaml::Thickness theme_thickness_th(wchar_t const* key, winrt::Microsoft::UI::Xaml::Thickness fallback) {\n";
    out << "    if (!key) return fallback;\n";
    out << "    std::wstring_view k{ key };\n";
    out << "    if (k.find(L\"ControlExampleDisplayBorderThickness\") != std::wstring_view::npos) {\n";
    out << "        return winrt::Microsoft::UI::Xaml::Thickness{0,0,0,0};\n";
    out << "    }\n";
    out << "    return fallback;\n";
    out << "}\n\n";

    return out.str();
}

// Generate the .ixx module for a single view.
// Pure modules — zero #include, zero ref new, zero raw pointers.
inline std::string generate_view_module(layout_view const& view, attribute_catalog const& catalog, bool const have_windows_sdk = false,
    std::set<std::string> const& projected_names = {})
{
    std::ostringstream out;
    std::string ns_meta = view.namespace_name.empty() ? "App" : view.namespace_name;
    // Identifier spelling (module declarations, imports, C++ namespaces):
    // the metadata namespace keeps the hyphen naming default (WinUI-Gallery)
    // while C++ requires an identifier - mangle here, keep ns_meta for the
    // activation string literals below.
    std::string ns = ident_ns(ns_meta);
    std::string class_name = view.name;

    // Sanitize: strip C-style comment markers that leak from XAML names
    {
        auto pos = class_name.find("/*");
        while (pos != std::string::npos) {
            auto end = class_name.find("*/", pos + 2);
            if (end != std::string::npos)
                class_name.erase(pos, end + 2 - pos);
            else
                class_name.erase(pos, 2);
            pos = class_name.find("/*", pos);
        }
        auto s = class_name.find_first_not_of(" \t\n\r");
        auto e = class_name.find_last_not_of(" \t\n\r");
        if (s != std::string::npos) class_name = class_name.substr(s, e - s + 1);
    }

    // View base comes from the winmd's TypeDef Extends hint (bare framework
    // name - projection-agnostic). Map it to the C++ platform type; fall back
    // to the classic name-based inference for legacy winmds.
    std::string base_type = "Microsoft::UI::Xaml::Controls::UserControl";
    bool is_app_class = false;
    bool is_window_class = false;
    // App-shell: an Application-based view that also hosts the main window
    // body. It emits both the AppHost machinery and the window class.
    bool is_shell = view.shell_window;
    if (is_shell) { is_app_class = false; is_window_class = true; }
    if (!view.base_type.empty())
    {
        if (view.base_type == "Application") base_type = "Microsoft::UI::Xaml::Application";
        else if (view.base_type == "Window") base_type = "Microsoft::UI::Xaml::Window";
        else if (view.base_type == "FrameworkElement" || view.base_type == "UIElement"
                 || view.base_type == "DependencyObject")
            base_type = "Microsoft::UI::Xaml::" + view.base_type;
        else
            base_type = "Microsoft::UI::Xaml::Controls::" + view.base_type;
    }
    is_app_class = view.base_type == "Application";
    is_window_class = view.base_type == "Window";
    if (is_shell) { is_app_class = false; is_window_class = true; }
    std::string main_window_access_prefix;
    if (!view.application_class.empty() && !view.main_window_class.empty())
        main_window_access_prefix = view.application_class + "." + view.main_window_class + ".";
    bool uses_main_window_access = false;
    if (!main_window_access_prefix.empty() && view.name != view.main_window_class)
        for (auto const& [callback, statements] : view.callbacks)
            for (auto const& statement : statements)
                if (statement.find(main_window_access_prefix) != std::string::npos)
                    uses_main_window_access = true;

    // Control has no content/children property — a Control-derived view with
    // a real body (its ControlTemplate lowered into the view by xaml2winuic)
    // must host its root element, so project it as ContentControl. Content
    // attaches exactly where the XAML template renders; a bare Control
    // cannot attach children at all (would leave the control empty again).
    if (view.base_type == "Control" && !view.root.empty())
        base_type = "Microsoft::UI::Xaml::Controls::ContentControl";

    // ── Module header ──
    // Platform namespaces named by this view's state-var / element types that
    // fall OUTSIDE the fixed import set below (e.g. Charts: Samples,
    // CategoryAxis, LinearAxis; InkControls; Tabular). g_type_ns resolves a
    // bare winmd type name to its namespace. Without the import the bare
    // to_cxx_type names don't resolve (C3646) and the -winui needed-set scan
    // — which reads these very import lines — never keeps the module reachable.
    std::vector<std::string> extra_platform_ns;
    {
        std::set<std::string, std::less<>> seen;
        auto add_extra = [&](std::string const& type_name) {
            if (type_name.size() < 2 || !std::isupper(static_cast<unsigned char>(type_name[0]))) return;
            if (type_name.find(':') != std::string::npos) return;   // already qualified
            auto it = g_type_ns.find(type_name);
            if (it == g_type_ns.end()) return;
            std::string const& xns = it->second;
            // Platform namespaces come from ANY loaded winmd — Microsoft.*,
            // Windows.* and third-party control libraries alike. The old
            // Microsoft./Windows. prefix gate emitted no import for a
            // third-party control the project uses, so its bare type name
            // never resolved (C3646) and the -winui needed-set scan — which
            // reads these very import lines — never kept its module alive.
            if (!is_platform_ns(xns)) return;
            // Namespaces already covered by the fixed import/using set below.
            static const std::set<std::string, std::less<>> core = {
                "Microsoft.UI.Xaml", "Microsoft.UI.Xaml.Hosting", "Microsoft.UI.Dispatching",
                "Microsoft.UI.Xaml.Controls", "Microsoft.UI.Xaml.Controls.Primitives",
                "Microsoft.UI.Xaml.Media", "Microsoft.UI.Xaml.Documents",
                "Microsoft.UI.Xaml.Media.Animation", "Microsoft.UI.Xaml.Shapes",
                "Microsoft.UI.Xaml.Input", "Microsoft.UI.Xaml.Data", "Microsoft.UI.Xaml.Markup",
                "Microsoft.UI.Windowing", "Microsoft.UI.Content", "Microsoft.UI.Input",
                "Windows.UI.Xaml.Interop", "Windows.Foundation", "Windows.Foundation.Collections",
                "Windows.UI", "Windows.UI.Text", "Windows.UI.Xaml" };
            if (core.contains(xns)) return;
            if (seen.insert(xns).second) extra_platform_ns.push_back(xns);
        };
        for (auto const& sv : view.state_vars) add_extra(sv.type);
        std::function<void(layout_node const&)> walk_ns = [&](layout_node const& nd) {
            add_extra(nd.control);
            for (auto const& ch : nd.children) walk_ns(ch);
        };
        for (auto const& rn : view.root) walk_ns(rn);
        for (auto const& [callback, statements] : view.callbacks) {
            (void)callback;
            for (auto const& statement : statements) {
                bool in_string = false;
                bool escaped = false;
                for (std::size_t p = 0; p < statement.size();) {
                    char ch = statement[p];
                    if (in_string) {
                        if (escaped) escaped = false;
                        else if (ch == '\\') escaped = true;
                        else if (ch == '"') in_string = false;
                        ++p;
                        continue;
                    }
                    if (ch == '"') { in_string = true; ++p; continue; }
                    if (!(std::isalpha(static_cast<unsigned char>(ch)) || ch == '_')) {
                        ++p;
                        continue;
                    }
                    std::size_t end = p + 1;
                    while (end < statement.size()
                        && (std::isalnum(static_cast<unsigned char>(statement[end])) || statement[end] == '_')) ++end;
                    if (end < statement.size() && statement[end] == '.') {
                        std::string type_name = statement.substr(p, end - p);
                        if (auto it = g_type_ns.find(type_name); it != g_type_ns.end())
                            add_extra(type_name);
                    }
                    p = end;
                }
            }
        }
    }

    // ── Module header ──
    out << "// " << class_name << ".ixx \u2014 module " << ns << "." << class_name << ".\n";
    out << "// generated by cppwinc from winuic-produced .winmd.\n\n";
    out << "module;\n\n";
    // No Win32/WinRT/WDK headers: everything this module needs comes from
    // winrt_base (imported below) or the standard library (`import std;`).
    out << "export module " << ns << "." << class_name << ";\n\n";
    out << "import std;\n";
    out << "export import winui_base;\n";
    for (auto const& xns : extra_platform_ns)
        out << "import winrt." << xns << ";\n";

    // Navigation windows import the page modules they navigate to
    if (is_window_class && !view.nav_pages.empty())
    {
        std::set<std::string> seen_page_mods;
        for (auto const& [tag, page] : view.nav_pages)
        {
            if (page.empty() || !seen_page_mods.insert(page).second) continue;
            out << "import " << ident_ns(page) << ";\n";
        }
    }
    if (is_app_class || is_shell) {
        out << "import winrt.Microsoft.UI.Xaml.Markup;\n";
        if (is_app_class && !is_shell && view.has_main_window && !view.main_window_class.empty())
            out << "import " << ns << "." << view.main_window_class << ";\n";
    }
    if (is_shell)
    {
        for (auto const& vn : g_view_type_names)
            if (vn != class_name) out << "import " << ns << "." << vn << ";\n";
    }
    for (auto const& comp_imp : view.component_view_imports)
        out << "import " << ident_ns(comp_imp) << ";\n";

    out << "using namespace winrt;\n";
    out << "using namespace winrt::" << ns << ";\n";
    out << "using namespace winrt::Microsoft::UI::Xaml;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Controls;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Documents;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Media;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Media::Animation;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Shapes;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Input;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Data;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Automation;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Automation::Peers;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Markup;\n";
    out << "using namespace winrt::Windows::Foundation;\n";
    out << "using namespace winrt::Windows::UI;\n";
    out << "using namespace winrt::CommunityToolkit::WinUI::Controls;\n";
    for (auto const& xns : extra_platform_ns)
    {
        if (xns == "Windows.UI.Core.AnimationMetrics") continue;
        if (xns == "Microsoft.UI.Xaml.Controls.AnimatedVisuals") continue;
        std::string colons;
        colons.reserve(xns.size() + 8);
        for (char ch : xns)
        {
            if (ch == '.') colons += "::";
            else colons += ch;
        }
        out << "using namespace winrt::" << colons << ";\n";
    }

    // ── Class definition ──
    // App class is exported (main.ixx needs to call Start()).
    // Other view classes are non-exported to avoid collision with component structs.
    // Window view classes get a <Name>View C++ name: the projected component
    // module exports a same-named wrapper struct and the view module does
    // 'using namespace winrt::<Ns>;' — an identical local class name is an
    // ambiguous lookup (C2872). The exported launcher keeps the public name.
    std::string cpp_class = class_name;
    // All view classes get a <Name>View C++ name: the component module
    // exports same-named wrapper structs (windows AND pages), so an
    // identical local class name is an ambiguous lookup (C2872) wherever
    // the bare name is used (factories, ctors).
    if (!is_app_class)
        cpp_class += "View";
    // Shell windows: the window class lives in the App view's module. Name
    // it <View>View so it can't collide with the component module's wrapper
    // struct (same rule as page views).
    std::string shell_cpp_class;
    if (is_shell)
    {
        shell_cpp_class = class_name + "ShellView";
        cpp_class = shell_cpp_class;
    }
    // Element names (x:Name) present in this view - collected before the
    // ctor blocks (which gate on membership) and before the full typed
    // named_elements map is built below.
    std::set<std::string> element_names;
    {
        std::function<void(layout_node const&)> collect_names = [&](layout_node const& nd) {
            for (auto const& [rk, rv] : nd.raw_attrs)
            {
                if (rk != "x:Name" || rv.empty()) continue;
                std::string nm = rv;
                if (nm.size() >= 2 && nm.front() == '"' && nm.back() == '"')
                    nm = nm.substr(1, nm.size() - 2);
                if (!nm.empty()) element_names.insert(nm);
            }
            for (auto const& ch : nd.children) collect_names(ch);
            for (auto const& ch : nd.else_branch) collect_names(ch);
        };
        for (auto const& rn : view.root) collect_names(rn);
    }
    auto find_named_element_by_control = [&](std::string_view target) -> std::string {
        std::string wanted(target);
        for (auto& ch : wanted) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::string result;
        std::function<void(layout_node const&)> find = [&](layout_node const& node) {
            if (!result.empty()) return;
            std::string control = node.control;
            auto sep = control.find_last_of(".:");
            if (sep != std::string::npos) control.erase(0, sep + 1);
            for (auto& ch : control) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (control == wanted) {
                for (auto const& [key, value] : node.raw_attrs) {
                    if ((key == "x:Name" || key == "Name") && !value.empty()) {
                        result = value;
                        if (result.size() >= 2 && result.front() == '"' && result.back() == '"')
                            result = result.substr(1, result.size() - 2);
                        return;
                    }
                }
            }
            for (auto const& child : node.children) find(child);
            for (auto const& child : node.else_branch) find(child);
        };
        for (auto const& node : view.root) find(node);
        return result;
    };
    if (is_app_class || is_shell)
    {
        // ── Local Application host: hand-rolled aggregated WinRT object (raw COM).
        // WinUI 3 startup contract: the ApplicationInitializationCallback must
        // CREATE the Application-derived runtime object - XAML creates none
        // itself. AppHost aggregates XAML's inner Application via
        // IApplicationFactory.CreateInstance(this-outer, &inner, &instance) and
        // provides IApplicationOverrides.OnLaunched, where the first window is
        // created and activated (the winui3-without-xaml pattern).
        // Shell views emit the same host machinery: their module IS the App
        // module, plus a window class below.
        if (is_shell)
            out << "export void launch_" << class_name << "();\n\n"; // defined at module bottom; used by on_launched
        out << "// ── Local Application host (aggregated WinRT outer object) ──\n";
        out << "struct AppHost\n{\n";
        out << "    struct app_inspectable_vtbl_t\n    {\n";
        out << "        std::int32_t (__stdcall* qi)(void*, winrt::winrt_guid const&, void**);\n";
        out << "        std::uint32_t (__stdcall* addref)(void*);\n";
        out << "        std::uint32_t (__stdcall* release)(void*);\n";
        out << "        std::int32_t (__stdcall* getiids)(void*, std::uint32_t*, winrt::winrt_guid**);\n";
        out << "        std::int32_t (__stdcall* getname)(void*, void**);\n";
        out << "        std::int32_t (__stdcall* gettrust)(void*, std::int32_t*);\n";
        out << "    };\n";
        out << "    struct app_overrides_vtbl_t : app_inspectable_vtbl_t\n    {\n";
        out << "        std::int32_t (__stdcall* onlaunched)(void*, void*);\n";
        out << "    };\n\n";
        // Stable WinUI 3 ABI GUIDs (Microsoft.UI.Xaml.winmd).
        out << "    static constexpr winrt::winrt_guid k_unknown_iid{0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};\n";
        out << "    static constexpr winrt::winrt_guid k_inspectable_iid{0xAF86E2E0,0xB12D,0x4C6A,{0x9C,0x5A,0xD7,0xAA,0x65,0x10,0x1E,0x90}};\n";
        out << "    static constexpr winrt::winrt_guid k_app_overrides_iid{0xA33E81EF,0xC665,0x503B,{0x88,0x27,0xD2,0x7E,0xF1,0x72,0x0A,0x06}};\n";
        out << "    static constexpr winrt::winrt_guid k_app_factory_iid{0x9FD96657,0x5294,0x5A65,{0xA1,0xDB,0x4F,0xEA,0x14,0x35,0x97,0xDA}};\n";
        // IXamlMetadataProvider (Microsoft.UI.Xaml.Markup) — XAML queries the
        // Application instance for this during XAML parsing (runtime source
        // MetadataAPI.cpp TryGetMetadataProvider: "try the Application
        // instance"). Without it, XamlControlsResources' internal
        // themeresources.xaml parse fails (Element not found) and the app
        // fail-fasts with stowed exceptions.
        out << "    static constexpr winrt::winrt_guid k_xmp_iid{0xA96251F0,0x2214,0x5D53,{0x87,0x46,0xCE,0x99,0xA2,0x59,0x3C,0xD7}};\n\n";
        out << "    static std::int32_t __stdcall abi_qi(void* self, winrt::winrt_guid const& iid, void** out) noexcept\n    {\n";
        out << "        if (!out) { return winrt::hr_pointer; }\n";
        out << "        *out = nullptr;\n";
        out << "        AppHost* host = static_cast<AppHost*>(self);\n";
        out << "        if (std::memcmp(&iid, &k_unknown_iid, sizeof(winrt::winrt_guid)) == 0\n";
        out << "            || std::memcmp(&iid, &k_inspectable_iid, sizeof(winrt::winrt_guid)) == 0)\n        {\n";
        out << "            *out = const_cast<void*>(static_cast<void const*>(&host->m_unk_vt));\n";
        out << "            host->abi_addref(self);\n";
        out << "            return 0;\n        }\n";
        out << "        if (std::memcmp(&iid, &k_app_overrides_iid, sizeof(winrt::winrt_guid)) == 0)\n        {\n";
        out << "            *out = const_cast<void*>(static_cast<void const*>(&host->m_ov_vt));\n";
        out << "            host->abi_addref(self);\n";
        out << "            return 0;\n        }\n";
        out << "        if (std::memcmp(&iid, &k_xmp_iid, sizeof(winrt::winrt_guid)) == 0)\n        {\n";
        out << "            // Pure delegation: hand XAML the real provider's interface\n";
        out << "            // pointer. No custom vtable, no slot math, no self-offset\n";
        out << "            // hazards (a tear-off field address is NOT the host base).\n";
        out << "            void* xmp = host->ensure_xmp();\n";
        out << "            boot_logf(\"qi:xmp-branch host=%p xmp=%p refs=%u\", (void*)host, xmp, host->m_refs);\n";
        out << "            if (!xmp) { boot_log(\"qi:xmp-null\"); return winrt::hr_no_interface; }\n";
        out << "            // Transfer: ensure_xmp's activation ref IS the caller's +1.\n";
        out << "            *out = xmp;\n";
        out << "            return 0;\n        }\n";
        out << "        if (host->m_inner)\n        {\n";
        out << "            // Forward to the inner Application. The depth guard breaks any\n";
        out << "            // pUnkOuter round-trip for interfaces neither side implements.\n";
        out << "            static thread_local std::int32_t s_depth = 0;\n";
        out << "            if (s_depth > 0) { return winrt::hr_no_interface; }\n";
        out << "            ++s_depth;\n";
        out << "            auto* ivt = *reinterpret_cast<void* const**>(host->m_inner);\n";
        out << "            auto inner_qi = reinterpret_cast<std::int32_t(__stdcall*)(void*, winrt::winrt_guid const&, void**)>(ivt[0]);\n";
        out << "            std::int32_t hr = inner_qi(host->m_inner, iid, out);\n";
        out << "            --s_depth;\n";
        out << "            return hr;\n        }\n";
        out << "        return winrt::hr_no_interface;\n    }\n";
        out << "    static std::uint32_t __stdcall abi_addref(void* self) noexcept\n    {\n";
        out << "        return ++static_cast<AppHost*>(self)->m_refs;\n    }\n";
        out << "    static std::uint32_t __stdcall abi_release(void* self) noexcept\n    {\n";
        out << "        AppHost* host = static_cast<AppHost*>(self);\n";
        out << "        std::uint32_t remaining = --host->m_refs;\n";
        out << "        boot_logf(\"release-hit host=%p remaining=%u\", (void*)host, remaining);\n";
        out << "        if (remaining == 0) { boot_log(\"release-kills-host\"); delete host; }\n";
        out << "        return remaining;\n    }\n";
        out << "    static std::int32_t __stdcall abi_getiids(void*, std::uint32_t* count, winrt::winrt_guid** iids) noexcept\n    {\n";
        out << "        if (count) { *count = 0; }\n";
        out << "        if (iids) { *iids = nullptr; }\n";
        out << "        return 0;\n    }\n";
        out << "    static std::int32_t __stdcall abi_getname(void*, void** name) noexcept\n    {\n";
        out << "        if (name) { *name = nullptr; }\n";
        out << "        return 0;\n    }\n";
        out << "    static std::int32_t __stdcall abi_gettrust(void*, std::int32_t* level) noexcept\n    {\n";
        out << "        if (level) { *level = 0; }\n";
        out << "        return 0;\n    }\n";
        // IApplicationOverrides is QI'd through the tear-off pointer &m_ov_vt
        // (second member, offset sizeof(void*) after m_unk_vt — standard
        // layout, no vptr). Its qi/addref/release thunks must translate self
        // back to the AppHost base before touching members; getiids, getname
        // and gettrust ignore self and are shared as-is.
        out << "    static AppHost* ov_host(void* self) noexcept\n    {\n";
        out << "        return reinterpret_cast<AppHost*>(static_cast<char*>(self) - sizeof(void*));\n";
        out << "    }\n";
        out << "    static std::int32_t __stdcall abi_ov_qi(void* self, winrt::winrt_guid const& iid, void** out) noexcept\n    {\n";
        out << "        return abi_qi(ov_host(self), iid, out);\n";
        out << "    }\n";
        out << "    static std::uint32_t __stdcall abi_ov_addref(void* self) noexcept\n    {\n";
        out << "        return abi_addref(ov_host(self));\n";
        out << "    }\n";
        out << "    static std::uint32_t __stdcall abi_ov_release(void* self) noexcept\n    {\n";
        out << "        return abi_release(ov_host(self));\n";
        out << "    }\n";
        out << "    static std::int32_t __stdcall abi_onlaunched(void* self, void*) noexcept\n    {\n";
        out << "        try { ov_host(self)->on_launched(); return 0; }\n";
        out << "        catch (...) { return winrt::hr_error; }\n";
        out << "    }\n\n";
        out << "    inline static const app_inspectable_vtbl_t s_unk_vtbl{\n";
        out << "        &AppHost::abi_qi, &AppHost::abi_addref, &AppHost::abi_release,\n";
        out << "        &AppHost::abi_getiids, &AppHost::abi_getname, &AppHost::abi_gettrust};\n";
        out << "    inline static const app_overrides_vtbl_t s_ov_vtbl{\n";
        out << "        &AppHost::abi_ov_qi, &AppHost::abi_ov_addref, &AppHost::abi_ov_release,\n";
        out << "        &AppHost::abi_getiids, &AppHost::abi_getname, &AppHost::abi_gettrust,\n";
        out << "        &AppHost::abi_onlaunched};\n";
        out << "    void const* m_unk_vt = nullptr;  // must stay first: this object IS an IUnknown\n";
        out << "    void const* m_ov_vt = nullptr;   // IApplicationOverrides tear-off\n";
        out << "    void* m_inner = nullptr;         // XAML Application inner (ref held)\n";
        out << "    void* m_instance = nullptr;      // composed Application (ref held)\n";
        out << "    void* m_xmp = nullptr;           // unused legacy slot (XMP is no longer cached)\n";
        out << "    std::uint32_t m_refs = 1;        // our own process-lifetime reference\n\n";
        // Lazily activate a FRESH controls metadata provider per query and
        // transfer the activation reference. Do NOT cache: XAML's metadata
        // subsystem adopts/destroys provider instances on its own schedule
        // (the eagerly-created one was released under us between boot and
        // the first XAML metadata query — use-after-free in abi_qi). A
        // fresh activation per QI always hands XAML a live object.
        // AppHost markers forward to the real winrt::boot_log: the startup
        // path (Application factory / CreateInstance / OnLaunched / XAML
        // resource merge) is otherwise silent, and an early REGDB failure
        // there produced zero boot_log context for diagnosis.
        out << "    static void boot_log(char const* m) { winrt::boot_log(m); }\n";
        out << "    template <typename... A>\n";
        out << "    static void boot_logf(char const*, A...) {}\n";
        out << "    void* ensure_xmp() noexcept\n    {\n";
        out << "        try\n        {\n";
        out << "            void* xf = winrt::impl::get_factory(L\"Microsoft.UI.Xaml.XamlTypeInfo.XamlControlsXamlMetaDataProvider\");\n";
        out << "            auto* xfvt = *reinterpret_cast<void* const**>(xf);\n";
        out << "            auto activate = reinterpret_cast<std::int32_t(__stdcall*)(void*, void**)>(xfvt[6]);\n";
        out << "            void* xinst = nullptr;\n";
        out << "            std::int32_t hr = activate(xf, &xinst);\n";
        out << "            auto* xfrvt = *reinterpret_cast<void* const**>(xf);\n";
        out << "            reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(xfrvt[2])(xf);\n";
        out << "            winrt::check_hresult(hr);\n";
        out << "            winrt::check_hresult(winrt::impl::query_interface(xinst, k_xmp_iid, &xinst));\n";
        out << "            return xinst;\n";
        out << "        }\n";
        out << "        catch (...) { boot_log(\"xmp:catch\"); return nullptr; }\n";
        out << "    }\n";
        out << "    AppHost()\n    {\n";
        out << "        m_unk_vt = &s_unk_vtbl;\n";
        out << "        m_ov_vt = &s_ov_vtbl;\n";
        out << "        boot_log(\"ctor:begin\");\n";
        out << "        // Compose: CreateInstance(outer=this, out inner, out instance).\n";
        out << "        // Raw pointers + explicit releases (see ensure_xmp note).\n";
        out << "        void* factory = winrt::impl::get_factory(L\"Microsoft.UI.Xaml.Application\");\n";
        out << "        boot_log(\"ctor:factory-ok\");\n";
        out << "        void* f = nullptr;\n";
        out << "        winrt::check_hresult(winrt::impl::query_interface(factory, k_app_factory_iid, &f));\n";
        out << "        boot_log(\"ctor:qi-ok\");\n";
        out << "        auto* fvt = *reinterpret_cast<void* const**>(f);\n";
        out << "        auto create = reinterpret_cast<std::int32_t(__stdcall*)(void*, void*, void**, void**)>(fvt[6]);\n";
        out << "        void* inner = nullptr;\n";
        out << "        void* instance = nullptr;\n";
        out << "        winrt::check_hresult(create(f, const_cast<void*>(static_cast<void const*>(&m_unk_vt)), &inner, &instance));\n";
        out << "        boot_log(\"ctor:create-ok\");\n";
        out << "        auto* frel = *reinterpret_cast<void* const**>(f);\n";
        out << "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(frel[2])(f);\n";
        out << "        auto* farel = *reinterpret_cast<void* const**>(factory);\n";
        out << "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(farel[2])(factory);\n";
        out << "        m_inner = inner;\n";
        out << "        m_instance = instance;\n";
        // Activate the controls metadata provider lazily on first XAML
        // metadata query (see ensure_xmp — no eager creation, no caching).
        out << "    }\n";
        out << "    ~AppHost()\n    {\n";
        out << "        auto release_raw = [](void* p)\n        {\n";
        out << "            if (p) { auto* vt = *reinterpret_cast<void* const**>(p); reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vt[2])(p); }\n";
        out << "        };\n";
        out << "        release_raw(m_instance);\n";
        out << "        release_raw(m_inner);\n";
        out << "    }\n\n";
        out << "    void on_launched()\n    {\n";
        out << "        // WinUI controls' default styles: 1:1 with the original XAML app's\n";
        out << "        // App.xaml resource setup.\n";
        out << "        boot_log(\"onlaunched:begin\");\n";
        out << "        try\n        {\n";
        out << "            auto current = winrt::Microsoft::UI::Xaml::Application::get_Current();\n";
        out << "            boot_log(\"ol:cur-ok\");\n";
        // Diagnostics: WINUIC_LAYOUT_TRACE=1 makes XAML name the element that
        // causes a layout cycle (written to the debugger output).
        out << "            if (std::getenv(\"WINUIC_LAYOUT_TRACE\")) {\n";
        out << "                auto dbg = current.DebugSettings();\n";
        out << "                dbg.LayoutCycleTracingLevel(winrt::Microsoft::UI::Xaml::LayoutCycleTracingLevel::High);\n";
        out << "                dbg.LayoutCycleDebugBreakLevel(winrt::Microsoft::UI::Xaml::LayoutCycleDebugBreakLevel::High);\n";
        out << "            }\n";
        out << "            {\n";
        out << "                auto res = current.Resources();\n";
        out << "                boot_log(\"ol:res-ok\");\n";
        out << "                {\n";
        out << "                    auto md = res.MergedDictionaries();\n";
        out << "                    boot_log(\"ol:md-ok\");\n";
        out << "                    {\n";
        out << "                        boot_log(\"ol:xcr-pre\");\n";
        out << "                        void* xcr_raw = winrt::impl::create_instance(L\"Microsoft.UI.Xaml.Controls.XamlControlsResources\");\n";
        out << "                        boot_log(\"ol:xcr-raw-ok\");\n";
        out << "                        {\n";
        out << "                            winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources xcr = winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources::from_abi(xcr_raw);\n";
        out << "                            boot_log(\"ol:xcr-ok\");\n";
        out << "                            md.Append(xcr);\n";
        out << "                            boot_log(\"ol:append-ok\");\n";
        out << "                            // App-lifetime resource: detach so the\n                        // wrapper dtor never releases it (XAML owns the\n                        // dictionary ref; teardown-ordering inside the\n                        // framework faults if the activation ref drops\n                        // while the merged dictionary still parses).\n";
        out << "                            (void)xcr.m_unknown.detach();\n";
        out << "                        }\n";
                        // Dynamic metadata-driven resource dictionary integration:
                        // Discovered directly from WinMD TypeDefs & WinUIControlCatalogAttribute.
                        // Future controls and external libraries resolve without cppwinc rebuilds.
                        {
                            std::vector<std::string> dicts_to_merge;
                            for (auto const& rd : g_dynamic_resource_dictionaries)
                            {
                                if (rd != "Microsoft.UI.Xaml.Controls.XamlControlsResources")
                                    dicts_to_merge.push_back(rd);
                            }
                            if (dicts_to_merge.empty())
                            {
                                dicts_to_merge = {
                                    "Microsoft.UI.Xaml.Controls.Tabular.TabularControlsResources",
                                    "Microsoft.UI.Xaml.Controls.XamlChartsResources"
                                };
                            }
                            for (auto const& rd_name : dicts_to_merge)
                            {
                                out << "                        try {\n";
                                out << "                            void* rd_raw = winrt::impl::create_instance(L\"" << rd_name << "\");\n";
                                out << "                            if (rd_raw) {\n";
                                out << "                                auto rd = winrt::Microsoft::UI::Xaml::ResourceDictionary::from_abi(rd_raw);\n";
                                out << "                                md.Append(rd);\n";
                                out << "                                (void)rd.m_unknown.detach();\n";
                                out << "                            }\n";
                                out << "                        } catch (...) {}\n";
                            }
                        }
                        {
                            std::string app_res_xaml;
                            auto find_app_res = [&](auto const& self, layout_node const& node) -> void {
                                for (auto const& [k, v] : node.raw_attrs) {
                                    if (k == "Resource" || k == "Resources") {
                                        if (v.find("<ResourceDictionary") != std::string::npos || v.find("<x:String") != std::string::npos) {
                                            app_res_xaml = v;
                                            return;
                                        }
                                    }
                                }
                                for (auto const& ch : node.children) self(self, ch);
                            };
                            for (auto const& rn : view.root) find_app_res(find_app_res, rn);
                            if (!app_res_xaml.empty()) {
                                out << "                        apply_app_resources(current, LR\"winui_app_res(" << app_res_xaml << ")winui_app_res\");\n";
                            }
                        }
        out << "                        boot_log(\"ol:xcr-dtor-ok\");\n";
        out << "                    }\n";
        out << "                    boot_log(\"ol:md-dtor-ok\");\n";
        out << "                    // App-lifetime: detach every XAML-owned wrapper —\n                // releasing property-derived dictionaries here races the\n                // framework's own teardown (use-after-free in dtor).\n";
        out << "                    (void)(md.m_ptr = nullptr); // interface wrapper: raw ptr, app-lifetime\n";
        out << "                }\n";
        out << "                // App-lifetime: detach every XAML-owned wrapper —\n                // releasing property-derived dictionaries here races the\n                // framework's own teardown (use-after-free in dtor).\n";
        out << "                (void)res.m_unknown.detach();\n";
        out << "                boot_log(\"ol:res-dtor-ok\");\n";
        out << "            }\n";
        out << "            (void)current.m_unknown.detach();\n";
        out << "            boot_log(\"ol:cur-dtor-ok\");\n";
        out << "        }\n";
        out << "        catch (...) { boot_log(\"ol:catch\"); }\n";
        out << "        boot_log(\"onlaunched:styles-done\");\n";
        if (view.has_main_window && !view.main_window_class.empty())
        {
            // launch_<Class>() constructs the view (a real runtime Window via
            // the projected composable ctor), builds the UI tree and activates.
            out << "        boot_log(\"onlaunched:launching\");\n";
            out << "        launch_" << view.main_window_class << "();\n";
            out << "        boot_log(\"onlaunched:launched\");\n";
        }
        else
        {
            out << "        try { winrt::Microsoft::UI::Xaml::Window w; w.Activate(); } catch (...) {}\n";
        }
        out << "    }\n";
        out << "};\n\n";
        out << "inline AppHost* g_apphost = nullptr;\n\n";
        out << "export struct App\n{\n";
        out << "public:\n";
    }
    else if (!is_app_class && !is_shell)
    {
        out << "export class " << cpp_class << " : public " << base_type << "\n{\n";
        out << "public:\n";
        if (!is_window_class)
        {
            // Pages/UserControls: default ctor builds the tree. Without it a
            // directly-constructed page has an empty XAML object.
            out << "    " << cpp_class << "()\n    {\n";
            out << "        BuildUI();\n";
            out << "    }\n\n";
        }
    }
    // App class: boot entry
    // Shell mode too: the App struct (opened above) is closed here and the
    // module-local shell window class (carrying state vars + BuildUI) opens
    // in its place.
    if (is_app_class || is_shell) {
        out << "    static void Start()\n";
        out << "    {\n";
        // The callback MUST create the Application-derived object (XAML
        // creates none itself). AppHost aggregates XAML's inner Application
        // and provides IApplicationOverrides.OnLaunched, which builds and
        // activates the first window.
        out << "        winrt::Microsoft::UI::Xaml::Application::Start(\n";
        out << "            winrt::Microsoft::UI::Xaml::ApplicationInitializationCallback([](winrt::Microsoft::UI::Xaml::ApplicationInitializationCallbackParams const&)\n";
        out << "            {\n";
        out << "                g_apphost = new AppHost();\n";
        out << "            }));\n";
        // Application::Start pumps internally and returns when the app exits.
        out << "    }\n";
        if (is_shell)
        {
            // Close the App struct and open the module-local shell window
            // class: state vars + BuildUI below belong to it. Non-exported
            // (launch_App below is the module's public surface).
            out << "};\n\n";
            out << "class " << cpp_class << ";\n";
            out << "inline " << cpp_class << "* g_active_shell = nullptr;\n\n";
            out << "class " << cpp_class << " : public Microsoft::UI::Xaml::Window\n{\n";
            out << "public:\n";
            out << "    " << cpp_class << "()\n    {\n";
            out << "        winrt::boot_log(\"view:ctor-begin\");\n";
            out << "        g_active_shell = this;\n";
            out << "        g_theme_state.notify_theme_change = []() { if (g_active_shell) g_active_shell->update_bindings(); };\n";
            out << "        BuildUI();\n";
            out << "        winrt::boot_log(\"view:ctor-end\");\n";
            out << "    }\n\n";
        }
    }

    // Window class: constructor calls BuildUI
    // (Shell windows already got their ctor when the window class opened.)
    if (is_window_class && !is_shell) {
        out << "    " << cpp_class << "()\n    {\n";
        out << "        winrt::boot_log(\"view:ctor-begin\");\n";
        out << "        BuildUI();\n";
        // A navigation view with a Frame must wait until Loaded before
        // selecting an item; its selection callback can navigate the Frame.
        bool has_loaded_cb = false;
        for (auto const& [cbn, cbs] : view.callbacks)
            if (cbn.find("Loaded") != std::string::npos) { has_loaded_cb = true; break; }
        std::string nav_control = find_named_element_by_control("NavigationView");
        std::string frame_control = find_named_element_by_control("Frame");
        if (!view.nav_pages.empty() && !nav_control.empty() && frame_control.empty() && !has_loaded_cb)
        {
            out << "        {\n";
            out << "            auto items = " << nav_control << ".MenuItems();\n";
            out << "            if (items && items.get_Size() > 0)\n";
            out << "            {\n";
            out << "                auto first = items.GetAt(0);\n";
            out << "                if (first) " << nav_control << ".SelectedItem(first);\n";
            out << "            }\n";
            out << "        }\n";
        }
        out << "        winrt::boot_log(\"view:ctor-end\");\n";
        out << "    }\n\n";
    }



    // Type inference map: state vars re-typed as real WinRT controls
    // (Slider/CheckBox/...) by the binding scans below. Shared by the
    // member declarations and the binding resolver, so it must outlive
    // the state-vars block - declared here at function scope.
    std::unordered_map<std::string, std::string> inferred_types;
    std::unordered_map<std::string, std::string> element_named_types;

    // State variables
    if (!view.state_vars.empty())
    {
        // Type inference: scan all raw_attrs and binding expressions to determine actual control types
        // for state variables that are typed as "string" in JSON but are actually control references.
        // (inferred_types is declared at function scope above)
        std::function<void(layout_node const&)> scan_for_bindings = [&](layout_node const& nd) {
            for (auto const& [rk, rv] : nd.raw_attrs)
            {
                // Check for $var.prop binding pattern
                auto dollar = rv.find('$');
                while (dollar != std::string::npos)
                {
                    auto var_start = dollar + 1;
                    auto dot = rv.find('.', var_start);
                    if (dot != std::string::npos)
                    {
                        std::string var_name = rv.substr(var_start, dot - var_start);
                        std::string prop = rv.substr(dot + 1);
                        auto end = prop.find_first_of(" ,)}\")(;");
                        if (end != std::string::npos) prop = prop.substr(0, end);
                        while (!prop.empty() && prop.back() == '(') prop.pop_back();
                        while (!prop.empty() && prop.back() == ')') prop.pop_back();
                        std::string lo_prop(prop);
                        for (auto& c : lo_prop) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        std::string ctrl_type;
                        if (lo_prop == "ison" || lo_prop == "is_on") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch";
                        else if (lo_prop == "value") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Slider";
                        else if (lo_prop == "text") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::TextBlock";
                        else if (lo_prop == "ischecked") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::CheckBox";
                        else if (lo_prop == "selectedindex") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ComboBox";
                        else if (lo_prop == "selecteditem") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ComboBox";
                        else if (lo_prop == "isexpanded") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Expander";
                        else if (lo_prop == "cangoback") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Frame";
                        else if (lo_prop == "cangoforward") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Frame";
                        else if (lo_prop == "isreadonly") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::TextBox";
                        else if (lo_prop == "password") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::PasswordBox";
                        else if (lo_prop == "isopen") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Primitives::Popup";
                        if (!ctrl_type.empty())
                            inferred_types[var_name] = ctrl_type;
                    }
                    dollar = rv.find('$', dollar + 1);
                }
                // Also scan raw_attrs for varName.propertyName pattern (without $ prefix)
                {
                    auto dot = rv.find('.');
                    if (dot != std::string::npos && dot > 0)
                    {
                        std::string var_name = rv.substr(0, dot);
                        std::string prop = rv.substr(dot + 1);
                        auto end = prop.find_first_of(" ,)}\")(;");
                        if (end != std::string::npos) prop = prop.substr(0, end);
                        while (!prop.empty() && prop.back() == '(') prop.pop_back();
                        while (!prop.empty() && prop.back() == ')') prop.pop_back();
                        if (!var_name.empty() && std::isalpha(static_cast<unsigned char>(var_name[0])))
                        {
                            bool is_known_type = (var_name == "true" || var_name == "false" || var_name == "null" || var_name == "self");
                            if (!is_known_type)
                            {
                                std::string lo_prop2(prop);
                                for (auto& c : lo_prop2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                std::string ctrl_type2;
                                if (lo_prop2 == "cangoback" || lo_prop2 == "cangoforward") ctrl_type2 = "winrt::Microsoft::UI::Xaml::Controls::Frame";
                                else if (lo_prop2 == "ison" || lo_prop2 == "is_on") ctrl_type2 = "winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch";
                                else if (lo_prop2 == "ischecked") ctrl_type2 = "winrt::Microsoft::UI::Xaml::Controls::CheckBox";
                                else if (lo_prop2 == "selectedindex" || lo_prop2 == "selecteditem") ctrl_type2 = "winrt::Microsoft::UI::Xaml::Controls::ComboBox";
                                else if (lo_prop2 == "text") ctrl_type2 = "winrt::Microsoft::UI::Xaml::Controls::TextBlock";
                                else if (lo_prop2 == "value") ctrl_type2 = "winrt::Microsoft::UI::Xaml::Controls::Slider";
                                else if (lo_prop2 == "isexpanded") ctrl_type2 = "winrt::Microsoft::UI::Xaml::Controls::Expander";
                                if (!ctrl_type2.empty())
                                    inferred_types[var_name] = ctrl_type2;
                            }
                        }
                    }
                }
            }
            // Also scan props (property values may contain $ bindings)
            for (auto const& [pk, pv] : nd.props)
            {
                // Look for "varName.propertyName" pattern (binding without $ prefix)
                std::string lo_pk(pk);
                for (auto& c : lo_pk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                // Skip numeric/bool literals
                if (pv.empty() || pv[0] == '"' || pv[0] == '-' || std::isdigit(static_cast<unsigned char>(pv[0]))) continue;
                auto dot = pv.find('.');
                if (dot != std::string::npos && dot > 0)
                {
                    std::string var_name = pv.substr(0, dot);
                    std::string prop = pv.substr(dot + 1);
                    auto end = prop.find_first_of(" ,)}\")(;");
                    if (end != std::string::npos) prop = prop.substr(0, end);
                    while (!prop.empty() && prop.back() == '(') prop.pop_back();
                    while (!prop.empty() && prop.back() == ')') prop.pop_back();
                    // Only infer if this looks like a variable name (alphanumeric + underscore, not a keyword)
                    if (var_name.empty() || !std::isalpha(static_cast<unsigned char>(var_name[0]))) continue;
                    bool is_known_type = (var_name == "true" || var_name == "false" || var_name == "null" || var_name == "self");
                    if (is_known_type) continue;
                    std::string lo_prop(prop);
                    for (auto& c : lo_prop) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    std::string ctrl_type;
                    if (lo_prop == "ison" || lo_prop == "is_on") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch";
                    else if (lo_prop == "value") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Slider";
                    else if (lo_prop == "text") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::TextBlock";
                    else if (lo_prop == "ischecked") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::CheckBox";
                    else if (lo_prop == "selectedindex") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ComboBox";
                    else if (lo_prop == "selecteditem") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ComboBox";
                    else if (lo_prop == "isexpanded") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Expander";
                    else if (lo_prop == "cangoback") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Frame";
                    else if (lo_prop == "cangoforward") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Frame";
                    else if (lo_prop == "isreadonly") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::TextBox";
                    else if (lo_prop == "password") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::PasswordBox";
                    else if (lo_prop == "isopen") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Primitives::Popup";
                    else if (lo_prop == "isactive") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ProgressRing";
                    else if (lo_prop == "isenabled") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Control";
                    if (!ctrl_type.empty())
                        inferred_types[var_name] = ctrl_type;
                }
            }
            for (auto const& ch : nd.children) scan_for_bindings(ch);
        };
        for (auto const& rn : view.root) scan_for_bindings(rn);

        // Fallback: scan ALL values in ALL nodes for statevar.Method() patterns
        // This catches cases where the $ prefix isn't used but state vars are referenced
        std::function<void(layout_node const&)> scan_fallback = [&](layout_node const& nd) {
            auto check_val = [&](std::string const& val) {
                for (auto const& sv : view.state_vars) {
                    if (sv.type != "string") continue; // only upgrade string-typed vars
                    std::string prefix = sv.name + ".";
                    auto pos = val.find(prefix);
                    if (pos != std::string::npos) {
                        std::string after = val.substr(pos + prefix.size());
                        auto end = after.find_first_of(" ,)}\")(;\"");
                        if (end != std::string::npos) after = after.substr(0, end);
                        while (!after.empty() && (after.back() == '(' || after.back() == ')')) after.pop_back();
                        std::string lo_after(after);
                        for (auto& c : lo_after) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        std::string ctrl_type;
                        if (lo_after == "cangoback" || lo_after == "cangoforward" || lo_after == "goback" || lo_after == "goforward") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Frame";
                        else if (lo_after == "ison" || lo_after == "is_on") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch";
                        else if (lo_after == "ischecked") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::CheckBox";
                        else if (lo_after == "selectedindex" || lo_after == "selecteditem") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::ComboBox";
                        else if (lo_after == "text") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::TextBlock";
                        else if (lo_after == "value") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Slider";
                        else if (lo_after == "isexpanded") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Expander";
                        else if (lo_after == "isopen") ctrl_type = "winrt::Microsoft::UI::Xaml::Controls::Primitives::Popup";
                        if (!ctrl_type.empty())
                            inferred_types[sv.name] = ctrl_type;
                    }
                }
            };
            for (auto const& [rk, rv] : nd.raw_attrs) check_val(rv);
            for (auto const& [pk, pv] : nd.props) check_val(pv);
            for (auto const& act : nd.action) check_val(act);
            for (auto const& ch : nd.children) scan_fallback(ch);
        };
        for (auto const& rn : view.root) scan_fallback(rn);

        // A state/binding var whose name matches an x:Name'd element in the
        // tree is a handle to that element: type it as the element's real
        // WinRT type (ToggleButton, not the $checked-usage guess CheckBox),
        // mirroring the named-element member computation below.
        // element_named_types is declared at function scope above
        {
            auto dot_to_colons = [](std::string const& s) {
                std::string r;
                r.reserve(s.size() + 8);
                for (char c : s) { if (c == '.') r += "::"; else r += c; }
                return r;
            };
            std::function<void(layout_node const&)> scan_ename = [&](layout_node const& nd) {
                for (auto const& [rk, rv] : nd.raw_attrs)
                {
                    if (rk != "x:Name" || rv.empty()) continue;
                    std::string nm = rv;
                    if (nm.size() >= 2 && nm.front() == '\"' && nm.back() == '\"')
                        nm = nm.substr(1, nm.size() - 2);
                    if (nm.empty()) continue;
                    bool is_state = false;
                    for (auto const& sv : view.state_vars)
                        if (sv.name == nm) { is_state = true; break; }
                    if (!is_state) continue;
                    std::string ns = resolve_type_ns(nd.control);
                    bool platform_type = is_platform_ns(ns);
                    std::string cxx;
                    if (platform_type)
                    {
                        cxx = dot_to_colons(catalog.winui3_type(nd.control));
                        if (cxx.rfind("winrt::", 0) != 0)
                        {
                            if (cxx.find("::") != std::string::npos)
                                cxx = "winrt::" + cxx;
                            else
                            {
                                std::string mem_ns = catalog.winui3_namespace(nd.control);
                                auto mnit = g_type_ns.find(nd.control);
                                if (mnit != g_type_ns.end() && is_platform_ns(mnit->second))
                                    mem_ns = mnit->second;
                                cxx = "winrt::" + dot_to_colons(mem_ns) + "::" + cxx;
                            }
                        }
                    }
                    else
                    {
                        cxx = catalog.winui3_type(nd.control);
                        if (cxx.find("::") != std::string::npos || cxx.find("winrt:") != std::string::npos)
                            cxx = nd.control + "View";  // app view type: match the emitting module's class name
                    }
                    // same projection guard as named_elements above
                    {
                        if (cxx.rfind("winrt::", 0) == 0 && cxx.find("::") != std::string::npos)
                        {
                            std::string md = cxx.substr(7);
                            for (auto& c : md) if (c == ':') c = '.';
                            std::string clean;
                            for (char c : md) { if (c == '.' && !clean.empty() && clean.back() == '.') continue; clean += c; }
                            if (!clean.empty() && clean.back() == '.') clean.pop_back();
                            if (!projected_names.count(clean)) cxx = "winrt::IInspectable";
                        }
                    }
                    element_named_types[nm] = cxx;
                }
                for (auto const& ch : nd.children) scan_ename(ch);
            };
            for (auto const& rn : view.root) scan_ename(rn);
        }

        out << "    // state variables\n";
        // Deduplicate: if same name appears twice, prefer non-wstring type
        {
            std::map<std::string, layout_state_var> dedup;
            for (auto const& sv : view.state_vars)
            {
                auto it = dedup.find(sv.name);
                if (it == dedup.end()) { dedup[sv.name] = sv; continue; }
                std::string old_cxx = to_cxx_type(it->second.type);
                std::string new_cxx = to_cxx_type(sv.type);
                if (old_cxx == "winrt::hstring" && new_cxx != "winrt::hstring")
                    it->second = sv;
            }
            for (auto const& [__, sv] : dedup)
            {
                // The x:Name'd element is authoritative: the member must be
                // assignable from that element (ToggleButton), so prefer it
                // over any $usage inference (CheckBox from isChecked).
                std::string cxx_type = to_cxx_type(sv.type);
                if (cxx_type == sv.type) {
                    if (auto metadata_type = g_type_ns.find(sv.type); metadata_type != g_type_ns.end()) {
                        std::string normalized_namespace;
                        for (char ch : metadata_type->second)
                            normalized_namespace += ch == '.' ? "::" : std::string(1, ch);
                        if (!normalized_namespace.empty())
                            cxx_type = "winrt::" + normalized_namespace + "::" + sv.type;
                    }
                }
                auto en = element_named_types.find(sv.name);
                if (en != element_named_types.end())
                    cxx_type = en->second;
                else if (sv.type == "string" || cxx_type == "winrt::hstring")
                {
                    auto it = inferred_types.find(sv.name);
                    if (it != inferred_types.end()) cxx_type = it->second;
                }
                std::string cap_name = sv.name;
                if (!cap_name.empty()) cap_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap_name[0])));
                std::string state_type = sv.type;
                for (auto& ch : state_type)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                bool value_state = state_type == "string" || state_type == "int" || state_type == "double"
                    || state_type == "bool" || state_type == "color" || state_type == "any"
                    || state_type == "object" || state_type == "hstring" || state_type == "winrt::hstring"
                    || state_type == "boolean" || state_type == "integer" || state_type == "any[]"
                    || state_type == "object[]" || (state_type.size() > 2 && state_type.substr(state_type.size() - 2) == "[]");
                bool has_state_accessors = value_state && !inferred_types.count(sv.name)
                    && !element_named_types.count(sv.name);
                std::string var_name = cap_name == sv.name && has_state_accessors ? ("m_" + sv.name) : sv.name;
                out << "    " << cxx_type << " " << var_name;
                if (!sv.initial.empty()) {
                    // If type is winrt::hstring, ensure string literal uses L prefix
                    if (cxx_type == "winrt::hstring" && sv.initial.front() == '"' && sv.initial.find("L\"") == std::string::npos)
                        out << " = L" << sv.initial;
                    else
                        out << " = " << sv.initial;
                }
                else if (cxx_type.find("ObservableVector") != std::string::npos || cxx_type.find("IVector") != std::string::npos)
                {
                    out << " = winrt::single_threaded_observable_vector<winrt::IInspectable>()";
                }
                else if (var_name == "WinAppSdkVersion" || var_name == "m_WinAppSdkVersion")
                {
                    out << " = L\"Windows App SDK 2.5-experimental\"";
                }
                else if (var_name == "WinAppSdkChannel" || var_name == "m_WinAppSdkChannel")
                {
                    out << " = L\"Experimental\"";
                }
                else if (var_name == "HasWinAppSdkChannel" || var_name == "m_HasWinAppSdkChannel")
                {
                    out << " = true";
                }
                else if (var_name == "WinAppSdkChannelDescription" || var_name == "m_WinAppSdkChannelDescription")
                {
                    out << " = L\"This app uses an experimental release of the Windows App SDK to showcase upcoming features. Some of the APIs it demonstrates are experimental and might change or be removed in a future release.\"";
                }
                else if (cxx_type == "winrt::hstring" && (var_name.find("Visibility") != std::string::npos || var_name.find("visibility") != std::string::npos))
                {
                    out << " = L\"Visible\"";
                }
                else if (cxx_type == "bool")
                {
                    out << " = false";
                }
                out << ";\n";
            }
        }
        out << "\n";
    }

    // Named elements (x:Name) - expose as typed class members so code-behind
    // handler bodies can reference them (Control1Output.Text = ...).
    std::map<std::string, std::string> named_elements;  // x:Name -> C++ type
    {
        std::function<void(layout_node const&)> collect_named = [&](layout_node const& nd) {
            for (auto const& [rk, rv] : nd.raw_attrs)
            {
                if (rk != "x:Name") continue;
                if (rv.empty()) continue;
                std::string nm = rv;
                if (nm.size() >= 2 && nm.front() == '\"' && nm.back() == '\"')
                    nm = nm.substr(1, nm.size() - 2);
                if (nm.empty()) continue;
                // Skip names already taken by generated members (state
                // variables, bindings): a page that names a CheckBox the same
                // as an extracted @state var must keep the state member.
                bool name_taken = false;
                for (auto const& sv : view.state_vars)
                    if (sv.name == nm) { name_taken = true; break; }
                if (name_taken) continue;
                // Member type must match the element's real WinRT type
                // (same source as the `auto X = <Type>();` local constructor):
                // never to_cxx_type() - that falls back to winrt::hstring for
                // controls missing from its map (PathIcon, Grid, ...).
                auto dot_to_colons = [](std::string const& s) {
                    std::string r;
                    r.reserve(s.size() + 8);
                    for (char c : s) { if (c == '.') r += "::"; else r += c; }
                    return r;
                };
                std::string ns = resolve_type_ns(nd.control);
                bool platform_type = is_platform_ns(ns);
                std::string cxx;
                if (platform_type)
                {
                    // Platform control: fully qualify from the catalog.
                    cxx = dot_to_colons(catalog.winui3_type(nd.control));
                    if (cxx.rfind("winrt::", 0) != 0)
                    {
                        if (cxx.find("::") != std::string::npos)
                            cxx = "winrt::" + cxx;   // qualified but missing winrt:: prefix
                        else
                        {
                            // Prefer the real metadata namespace (Media/Data/Input/...)
                            // over the catalog default (Controls); the catalog default
                            // misfires for ThemeShadow(Media), CollectionViewSource(Data),
                            // XamlUICommand(Input).
                            std::string mem_ns = catalog.winui3_namespace(nd.control);
                            auto mnit = g_type_ns.find(nd.control);
                            if (mnit != g_type_ns.end() && is_platform_ns(mnit->second))
                                mem_ns = mnit->second;
                            cxx = "winrt::" + dot_to_colons(mem_ns) + "::" + cxx;
                        }
                    }
                }
                else
                {
                    // App/toolkit-defined control (e.g. gallery ControlExample):
                    // the module imports the component module + using-directives,
                    // so the <Name>View class name resolves exactly like the
                    // locals (`auto X = ControlExampleView();`). Never qualify
                    // as Microsoft.UI.Xaml.Controls::<name> - that type does
                    // not exist for app controls. The View suffix matches the
                    // emitting module's class name (cpp_class rule below).
                    cxx = catalog.winui3_type(nd.control);
                    if (cxx.find("::") != std::string::npos || cxx.find("winrt:") != std::string::npos)
                        cxx = nd.control + "View";  // catalog returned an unrelated FQ type; use the view class name
                }
                named_elements[nm] = cxx;
            }
            for (auto const& ch : nd.children) collect_named(ch);
        };
        for (auto const& rn : view.root) collect_named(rn);
        // Guard: a resolved type the projection does not carry (module pruned
        // from the needed set - CollectionViewSource, ...) cannot name a
        // member. Fall back to IInspectable; assignments still compile.
        auto ensure_projected = [&](std::string const& t) -> std::string {
            if (t.rfind("winrt::", 0) != 0 || t.find("::") == std::string::npos) return t;
            std::string md = t.substr(7);
            for (auto& c : md) if (c == ':') c = '.';
            // collapse the double dots produced by "::" -> ".."
            std::string clean;
            for (char c : md) { if (c == '.' && !clean.empty() && clean.back() == '.') continue; clean += c; }
            if (!clean.empty() && clean.back() == '.') clean.pop_back();
            return projected_names.count(clean) ? t : "winrt::IInspectable";
        };
        for (auto& [nm, t] : named_elements) t = ensure_projected(t);
        if (!named_elements.empty())
        {
            out << "    // named elements (x:Name) - referenced by code-behind handlers\n";
            for (auto const& [nm, t] : named_elements)
                out << "    " << t << " " << nm << ";\n";
            out << "\n";
        }
    }

    // Member names that shadow type names inside the class: state vars and
    // named elements. Element constructors for those types must be qualified
    // with the type's namespace (auto X = winrt::Ns::Type(); instead of
    // auto X = Type();) or `Type()` resolves to the member.
    std::set<std::string> shadow_types;
    for (auto const& sv : view.state_vars)
        shadow_types.insert(sv.name);
    for (auto const& [nm, t] : named_elements)
        shadow_types.insert(nm);

    auto has_elem = [&](std::string const& s) {
        if (named_elements.count(s)) return true;
        for (auto const& sv : view.state_vars) if (sv.name == s) return true;
        return false;
    };
    std::string const title_bar_name = find_named_element_by_control("TitleBar");
    std::string const nav_view_name = find_named_element_by_control("NavigationView");
    std::string const frame_name = find_named_element_by_control("Frame");
    std::string const search_box_name = find_named_element_by_control("AutoSuggestBox");
    bool const is_nav_window = is_window_class && !view.nav_pages.empty() && !nav_view_name.empty() && !frame_name.empty();

    std::set<std::string> catalog_source_vars;
    std::set<std::string> recently_visited_source_vars;
    std::set<std::string> all_controls_source_vars;
    std::set<std::string> recent_section_element_names;
    auto normalize_layout_value = [](std::string value) {
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"')
            || (value.front() == 'L' && value.size() >= 3 && value[1] == '"' && value.back() == '"')))
            value = value.substr(value.front() == 'L' ? 2 : 1, value.size() - (value.front() == 'L' ? 3 : 2));
        return value;
    };
    auto layout_prop_value = [](layout_node const& node, std::string_view wanted) {
        auto equals_prop = [&](std::string const& name) {
            std::string lowered = name;
            for (auto& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string target(wanted);
            for (auto& c : target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lowered == target;
        };
        for (auto const& [name, value] : node.props)
            if (equals_prop(name)) return value;
        for (auto const& [name, value] : node.raw_attrs)
            if (equals_prop(name)) return value;
        return std::string{};
    };
    std::function<void(std::vector<layout_node> const&, int, std::vector<std::string> const&)> find_catalog_sources;
    find_catalog_sources = [&](std::vector<layout_node> const& nodes, int in_sample_section, std::vector<std::string> const& named_ancestors) {
        int section_context = in_sample_section;
        for (auto const& node : nodes) {
            std::string text = normalize_layout_value(layout_prop_value(node, "Text"));
            if (!text.empty()) {
                std::string lowered = text;
                for (auto& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                bool is_recently_visited = lowered.find("recently visited") != std::string::npos;
                bool is_recently_updated = lowered.find("recently added") != std::string::npos
                    && lowered.find("updated") != std::string::npos;
                section_context = is_recently_visited ? 1 : (is_recently_updated ? 2 : 0);
                if (is_recently_visited) {
                    std::string heading_name = normalize_layout_value(layout_prop_value(node, "x:Name"));
                    if (heading_name.empty()) heading_name = normalize_layout_value(layout_prop_value(node, "Name"));
                    if (!heading_name.empty()) recent_section_element_names.insert(heading_name);
                }
            }
            if (section_context) {
                std::string source = normalize_layout_value(layout_prop_value(node, "ItemsSource"));
                if (!source.empty() && source.front() == '$') source.erase(source.begin());
                for (auto const& state : view.state_vars) {
                    if (state.name != source || !state.initial.empty()
                        || (state.type != "any[]" && state.type != "object[]" && (state.type.size() <= 2 || state.type.substr(state.type.size() - 2) != "[]")))
                        continue;
                    if (section_context == 1) {
                        recently_visited_source_vars.insert(state.name);
                        if (!named_ancestors.empty())
                            recent_section_element_names.insert(named_ancestors.back());
                    }
                    else if (section_context == 2)
                        catalog_source_vars.insert(state.name);
                }
            }
            auto child_ancestors = named_ancestors;
            std::string node_name = normalize_layout_value(layout_prop_value(node, "x:Name"));
            if (node_name.empty()) node_name = normalize_layout_value(layout_prop_value(node, "Name"));
            if (!node_name.empty()) child_ancestors.push_back(node_name);
            find_catalog_sources(node.children, section_context, child_ancestors);
        }
    };
    find_catalog_sources(view.root, false, {});
    if (view.name == "AllControlsPage" || view.name == "AllControls") {
        for (auto const& state : view.state_vars) {
            if (state.name == "items" || state.name == "Items")
                all_controls_source_vars.insert(state.name);
        }
    }

    // Collect unique callback names - use action name for unique per-control handlers
    std::vector<std::string> callbacks;
    std::set<std::string> callback_set;
    auto add_callback = [&](std::string const& name) {
        if (!name.empty() && callback_set.insert(name).second)
            callbacks.push_back(name);
    };
    std::function<void(layout_node const&)> collect_callbacks = [&](layout_node const& n)
    {
        for (auto const& ev : n.events)
        {
            if (!ev.actions.empty())
            {
                // Use action method name: "GoBackInvokerButtonClick()" → "GoBackInvokerButtonClick"
                std::string act = ev.actions[0];
                auto paren = act.find('(');
                if (paren != std::string::npos) act = act.substr(0, paren);
                while (!act.empty() && act.back() == '_') act.pop_back();
                add_callback(act);
            }
            else
            {
                // Fallback: derive from event name
                std::string evname = ev.name;
                if (evname.size() > 2 && evname[0] == 'o' && evname[1] == 'n')
                    evname = evname.substr(2);
                if (!evname.empty()) evname[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(evname[0])));
                add_callback("On" + evname);
            }
        }
        for (auto const& ch : n.children) collect_callbacks(ch);
    };
    for (auto const& rn : view.root) collect_callbacks(rn);

    // Function-call binding methods and one-way binding refresh list
    std::set<std::string> call_methods;
    std::vector<std::string> refresh_statements;

    // Event handler declarations — skip, definitions below serve as declarations

    // ── BuildUI() method ──
    std::string effective_backdrop = view.backdrop_type;
    // Pre-scan root nodes for MicaBackdrop / AcrylicBackdrop (may set effective_backdrop)
    for (auto const& rn : view.root)
    {
        if (rn.control == "MicaBackdrop" || rn.control == "SystemBackdropElement")
        {
            if (effective_backdrop.empty()) effective_backdrop = "mica";
        }
        else if (rn.control == "DesktopAcrylicBackdrop")
        {
            if (effective_backdrop.empty()) effective_backdrop = "acrylic";
        }
    }
    // BuildUI() — only for Window/UserControl classes (not App)
    if (!is_app_class) {
    out << "    // Reactive bindings\n";
    out << "    std::vector<std::function<void()>> m_bindings;\n";
    out << "    std::vector<std::shared_ptr<void>> m_child_views;\n";
    out << "    void update_bindings()\n    {\n";
    out << "        for (auto const& fn : m_bindings) fn();\n";
    if (is_shell)
        out << "        update_router();\n";
    out << "    }\n\n";

    auto has_state = [&](std::string_view name) {
        return std::any_of(view.state_vars.begin(), view.state_vars.end(),
            [&](layout_state_var const& state) {
                if (state.name.size() != name.size()) return false;
                for (std::size_t i = 0; i < state.name.size(); ++i)
                    if (std::tolower(static_cast<unsigned char>(state.name[i]))
                        != std::tolower(static_cast<unsigned char>(name[i]))) return false;
                return true;
            });
    };
    auto state_accessor_name = [&](std::string_view name) {
        auto state = std::find_if(view.state_vars.begin(), view.state_vars.end(),
            [&](layout_state_var const& candidate) {
                if (candidate.name.size() != name.size()) return false;
                for (std::size_t i = 0; i < candidate.name.size(); ++i)
                    if (std::tolower(static_cast<unsigned char>(candidate.name[i]))
                        != std::tolower(static_cast<unsigned char>(name[i]))) return false;
                return true;
            });
        if (state == view.state_vars.end()) return std::string(name);
        std::string accessor = state->name;
        if (!accessor.empty())
            accessor[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(accessor[0])));
        return accessor;
    };
    bool has_sample_definition = has_state("sampleDefinition");
    bool has_sample_header = has_state("headerText");
    bool has_sample_xaml = has_state("xaml");
    bool has_sample_csharp = has_state("cSharp");
    if (has_sample_definition && has_sample_header && has_sample_xaml && has_sample_csharp)
    {
        out << "    void _load_sample_definition(winrt::hstring const& reference)\n    {\n";
        out << "        std::filesystem::path relative(reference.c_str());\n";
        out << "        if (relative.empty() || relative.is_absolute()) { std::println(\"invalid sample definition path\"); return; }\n";
        out << "        for (auto const& part : relative) if (part == L\"..\") { std::println(\"sample definition path escapes Samples directory\"); return; }\n";
        out << "        std::ifstream input(std::filesystem::path(L\"Samples\") / relative, std::ios::binary);\n";
        out << "        if (!input) { std::println(\"could not open sample definition: {}\", relative.string()); return; }\n";
        out << "        std::string section, line, header, xaml, csharp, buffer;\n";
        out << "        auto flush = [&]() { if (section == \"header\") header = buffer; else if (section == \"xaml\") xaml = buffer; else if (section == \"c#\" || section == \"csharp\") csharp = buffer; buffer.clear(); };\n";
        out << "        while (std::getline(input, line)) {\n";
        out << "            if (!line.empty() && line.back() == '\\r') line.pop_back();\n";
        out << "            if (line.rfind(\"--- \", 0) == 0) { flush(); section = line.substr(4); for (std::size_t i = 0; i < section.size(); ++i) { auto& ch = section[i]; ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); } while (!section.empty() && (section.back() == ' ' || section.back() == '\\t')) section.pop_back(); }\n";
        out << "            else { if (!buffer.empty()) buffer += '\\n'; buffer += line; }\n";
        out << "        }\n";
        out << "        flush();\n";
        out << "        auto translated_path = std::filesystem::path(L\"Samples\") / L\"WinUI\" / relative;\n";
        out << "        translated_path.replace_extension(L\".winui\");\n";
        out << "        std::ifstream translated_input(translated_path, std::ios::binary);\n";
        out << "        if (translated_input) { std::string translated((std::istreambuf_iterator<char>(translated_input)), std::istreambuf_iterator<char>()); if (!translated.empty()) xaml = std::move(translated); }\n";
        out << "        this->" << state_accessor_name("headerText") << "(winrt::hstring(winrt::to_hstring(std::string_view(header))));\n";
        out << "        this->" << state_accessor_name("xaml") << "(winrt::hstring(winrt::to_hstring(std::string_view(xaml))));\n";
        out << "        this->" << state_accessor_name("cSharp") << "(winrt::hstring(winrt::to_hstring(std::string_view(csharp))));\n";
        out << "        update_bindings();\n";
        out << "    }\n\n";
    }

    // Property getters and setters for state variables
    auto is_color_state = [&](std::string const& name) {
        std::string type_key = view.name;
        auto separator = type_key.find_last_of(".:");
        if (separator != std::string::npos) type_key = type_key.substr(separator + 1);
        for (auto& ch : type_key)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::string property_key = name;
        for (auto& ch : property_key)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto properties = g_type_prop_kinds.find(type_key);
        if (properties == g_type_prop_kinds.end()) return false;
        auto property = properties->second.find(property_key);
        if (property == properties->second.end()) return false;
        std::string property_type = property->second;
        auto type_separator = property_type.find_last_of(".:");
        if (type_separator != std::string::npos) property_type = property_type.substr(type_separator + 1);
        for (auto& ch : property_type)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return property_type == "color";
    };
    for (auto const& sv : view.state_vars)
    {
        // Skip controls / named elements (x:Name controls like ActionButton, PasswordInput, Sliders)
        std::string cap_name = sv.name;
        if (!cap_name.empty()) cap_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap_name[0])));
        bool accessor_name_conflict = std::any_of(view.state_vars.begin(), view.state_vars.end(),
            [&](auto const& other) { return &other != &sv && other.name == cap_name; });
        if (accessor_name_conflict) continue;
        if (inferred_types.count(sv.name) || inferred_types.count(cap_name)) continue;
        if (element_named_types.count(sv.name) || element_named_types.count(cap_name)) continue;
        if (named_elements.count(sv.name) || named_elements.count(cap_name)) continue;

        // Only value properties (not controls/elements)
        std::string lo_t = sv.type;
        for (auto& c : lo_t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool is_val = (lo_t == "string" || lo_t == "int" || lo_t == "double" || lo_t == "bool" || lo_t == "color"
            || lo_t == "any" || lo_t == "object" || lo_t == "hstring" || lo_t == "winrt::hstring"
            || lo_t == "boolean" || lo_t == "integer" || lo_t == "any[]" || lo_t == "object[]"
            || (lo_t.size() > 2 && lo_t.substr(lo_t.size() - 2) == "[]"));
        if (!is_val) continue;

        std::string cxx_type = to_cxx_type(sv.type);
        if (cxx_type.find("Controls::") != std::string::npos) continue;

        std::string mem_var = (cap_name == sv.name && !named_elements.count(sv.name)) ? ("m_" + sv.name) : sv.name;
        out << "    void " << cap_name << "(" << cxx_type << " const& v) { " << mem_var << " = v;";
        if (sv.name == "sampleDefinition" && has_sample_definition)
            out << " _load_sample_definition(v);";
        out << " update_bindings(); }\n";
        out << "    " << cxx_type << " const& " << cap_name << "() const { return " << mem_var << "; }\n";
        if (cxx_type == "winrt::hstring")
        {
            out << "    void " << cap_name << "(winrt::IInspectable const& v) { " << mem_var << " = winrt::unbox_value_or<winrt::hstring>(v, L\"\"); update_bindings(); }\n";
            out << "    void " << cap_name << "(int v) { " << mem_var << " = winrt::hstring(winrt::to_hstring(v).c_str()); update_bindings(); }\n";
            out << "    void " << cap_name << "(double v) { " << mem_var << " = winrt::hstring(winrt::to_hstring(v).c_str()); update_bindings(); }\n";
            if (cap_name == "HorizontalContentAlignment")
                out << "    void " << cap_name << "(winrt::Microsoft::UI::Xaml::HorizontalAlignment const& v) { " << mem_var << " = (v == winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center ? L\"Center\" : (v == winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right ? L\"Right\" : (v == winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch ? L\"Stretch\" : L\"Left\"))); update_bindings(); }\n";
            if (cap_name == "VerticalContentAlignment")
                out << "    void " << cap_name << "(winrt::Microsoft::UI::Xaml::VerticalAlignment const& v) { " << mem_var << " = (v == winrt::Microsoft::UI::Xaml::VerticalAlignment::Center ? L\"Center\" : (v == winrt::Microsoft::UI::Xaml::VerticalAlignment::Bottom ? L\"Bottom\" : (v == winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch ? L\"Stretch\" : L\"Top\"))); update_bindings(); }\n";
            if (cap_name == "Padding" || cap_name == "BorderThickness" || cap_name == "Margin")
                out << "    void " << cap_name << "(winrt::Microsoft::UI::Xaml::Thickness const& v) { " << mem_var << " = winrt::hstring((winrt::to_hstring((int)v.Left) + L\",\" + winrt::to_hstring((int)v.Top) + L\",\" + winrt::to_hstring((int)v.Right) + L\",\" + winrt::to_hstring((int)v.Bottom)).c_str()); update_bindings(); }\n";
            if (cap_name == "CornerRadius")
                out << "    void " << cap_name << "(winrt::Microsoft::UI::Xaml::CornerRadius const& v) { " << mem_var << " = winrt::hstring((winrt::to_hstring((int)v.TopLeft) + L\",\" + winrt::to_hstring((int)v.TopRight) + L\",\" + winrt::to_hstring((int)v.BottomRight) + L\",\" + winrt::to_hstring((int)v.BottomLeft)).c_str()); update_bindings(); }\n";
            if (cap_name == "SourceCodeVisibility")
                out << "    void " << cap_name << "(winrt::Microsoft::UI::Xaml::Visibility const& v) { " << mem_var << " = (v == winrt::Microsoft::UI::Xaml::Visibility::Collapsed ? L\"Collapsed\" : L\"Visible\"); update_bindings(); }\n";
            if (cap_name == "Color")
                out << "    void " << cap_name << "(winrt::Windows::UI::Color const& v) { (void)v; update_bindings(); }\n";
        }
        else if (cxx_type == "winrt::IInspectable")
        {
            out << "    void " << cap_name << "(winrt::hstring const& v) { " << mem_var << " = winrt::box_value(v); update_bindings(); }\n";
            out << "    void " << cap_name << "(wchar_t const* v) { " << mem_var << " = winrt::box_value(winrt::hstring(v)); update_bindings(); }\n";
            out << "    void " << cap_name << "(int v) { " << mem_var << " = winrt::box_value(v); update_bindings(); }\n";
            out << "    void " << cap_name << "(double v) { " << mem_var << " = winrt::box_value(v); update_bindings(); }\n";
            if (is_color_state(sv.name))
                out << "    void " << cap_name << "(winrt::Windows::UI::Color const& v) { " << mem_var << " = winrt::box_value(v); update_bindings(); }\n";
        }
    }
    out << "\n";

    if (is_shell)
    {
        out << "    winrt::Microsoft::UI::Xaml::Controls::Grid m_router_panel;\n";
        out << "    void update_router()\n    {\n";
        out << "        if (!m_router_panel.get_abi()) return;\n";
        out << "        m_router_panel.Children().Clear();\n";
        out << "        auto theme = is_current_light_theme() ? winrt::Microsoft::UI::Xaml::ElementTheme::Light : winrt::Microsoft::UI::Xaml::ElementTheme::Dark;\n";
        for (size_t i = 0; i < view.nav_pages.size(); ++i)
        {
            std::string page_name = view.nav_pages[i].second;
            auto separator = page_name.rfind('.');
            if (separator != std::string::npos) page_name.erase(0, separator + 1);
            out << (i == 0 ? "        if" : "        else if") << " (selectedNav == " << i << ")\n";
            out << "        {\n";
            out << "            auto& page = get_" << page_name << "();\n";
            out << "            page.RequestedTheme(theme);\n";
            out << "            page.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch);\n";
            out << "            page.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);\n";
            out << "            m_router_panel.Children().Append(page);\n";
            out << "        }\n";
        }
        out << "    }\n\n";
    }

    out << "    // Build the UI tree from the winuic layout.\n";
    int tpl_counter = 0;
    std::vector<template_factory_result> pending_factory_funcs;
    out << "    void BuildUI()\n    {\n";
    out << "        boot_log(\"ui:view:" << escape_wstr(view.name) << "\");\n";
    out << "        boot_log(\"ui:begin\");\n";
    out << "#define WINUIC_UI_STEP(s) do { winrt::boot_log(s); } while (0)\n";
    out << "        std::vector<std::pair<winrt::Microsoft::UI::Xaml::DependencyObject, std::function<winrt::Microsoft::UI::Xaml::UIElement()>>> _deferred_labeled_by;\n";
    out << "        std::vector<std::function<void()>> _deferred_relative_panel;\n";



    // Load ResourceDictionary if present (styles, themes, converters)
    // ResourceDictionary: already embedded in resources.pri via winuic compiler.
    // No XAML file loading — the Zero XAML philosophy puts all resource data
    // directly into binary .winmd metadata and resources.pri.

    // Window-level directives (only for Window subclasses)
    if (is_window_class) {
        if (!view.dark_titlebar) const_cast<layout_view&>(view).dark_titlebar = true;
        out << "        g_theme_state.theme_idx = 2;\n";
        out << "        WINUIC_UI_STEP(\"ui:appid-begin\");\n";
        out << "        g_theme_state.window_id = this->winrt::Microsoft::UI::Xaml::Window::AppWindow().Id().Value;\n";
        out << "        g_theme_state.main_hwnd = reinterpret_cast<void*>(static_cast<std::uintptr_t>(this->winrt::Microsoft::UI::Xaml::Window::AppWindow().Id().Value));\n";
        out << "        WINUIC_UI_STEP(\"ui:appid-done\");\n";
        out << "        try {\n";
        out << "            void* win_native = nullptr;\n";
        out << "            static const winrt::winrt_guid iid_wn{0xEECDBF0E, 0xBAE9, 0x4CB6, {0xA6, 0x8E, 0x95, 0x98, 0xE1, 0xCB, 0x57, 0xBB}};\n";
        out << "            if (winrt::impl::query_interface(m_unknown.get(), iid_wn, &win_native) >= 0 && win_native) {\n";
        out << "                using get_hwnd_t = std::int32_t(__stdcall*)(void*, void**);\n";
        out << "                void* hwnd_val = nullptr;\n";
        out << "                auto* vt = *reinterpret_cast<void* const**>(win_native);\n";
        out << "                reinterpret_cast<get_hwnd_t>(vt[3])(win_native, &hwnd_val);\n";
        out << "                using rel_t = std::uint32_t(__stdcall*)(void*);\n";
        out << "                reinterpret_cast<rel_t>(vt[2])(win_native);\n";
        out << "                if (hwnd_val) g_theme_state.main_hwnd = hwnd_val;\n";
        out << "            }\n";
        out << "        } catch (...) { winrt::report_current_exception(\"item click\"); }\n";
        if (view.extend_content_into_titlebar)
            out << "        g_theme_state.is_extended_titlebar = true;\n";
        out << "        WINUIC_UI_STEP(\"ui:sync-titlebar\");\n";
        out << "        sync_titlebar_theme(true);\n";
        out << "        WINUIC_UI_STEP(\"ui:sync-titlebar-done\");\n";
        // WinUIExtendContentIntoTitleBarAttribute contract: extended content takes
        // priority over the default black/white system titlebar. It is emitted
        // FIRST and suppresses AppWindow titlebar theming that would paint a
        // system-colored strip behind the extended content. Dark mode in extended
        // mode adjusts caption glyph colors over the app content instead.
        if (view.extend_content_into_titlebar)
        {
            out << "        WINUIC_UI_STEP(\"ui:extend-titlebar\");\n";
            out << "        ExtendsContentIntoTitleBar(true);\n";
            out << "        WINUIC_UI_STEP(\"ui:extend-titlebar-done\");\n";
            out << "        auto appTitleBar = this->winrt::Microsoft::UI::Xaml::Window::AppWindow().TitleBar();\n";
            out << "        appTitleBar.PreferredHeightOption(winrt::Microsoft::UI::Windowing::TitleBarHeightOption::Tall);\n";
            if (view.dark_titlebar)
            {
                out << "        WINUIC_UI_STEP(\"ui:caption-colors\");\n";
                out << "        appTitleBar.ButtonForegroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});\n";
                out << "        appTitleBar.ButtonHoverForegroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});\n";
                out << "        appTitleBar.ButtonPressedForegroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});\n";
                out << "        appTitleBar.ButtonInactiveForegroundColor(winrt::Windows::UI::Color{255, 160, 160, 160});\n";
                out << "        appTitleBar.ButtonBackgroundColor(winrt::Windows::UI::Color{0, 0, 0, 0});\n";
                out << "        appTitleBar.ButtonHoverBackgroundColor(winrt::Windows::UI::Color{0, 0, 0, 0});\n";
                out << "        appTitleBar.ButtonPressedBackgroundColor(winrt::Windows::UI::Color{0, 0, 0, 0});\n";
                out << "        appTitleBar.ButtonInactiveBackgroundColor(winrt::Windows::UI::Color{0, 0, 0, 0});\n";
            }
        }
        else if (view.dark_titlebar)
        {
            // WinUIDarkTitlebarAttribute contract: DWM immersive dark mode
            // (attr 20) - AppWindowTitleBar has no PreferredTheme API.
            out << "        {\n";
            out << "            int dark = 1;\n";
            out << "            void* dwm = LoadLibraryExW(L\"dwmapi.dll\", nullptr, 0x00000800);\n";
            out << "            if (dwm) {\n";
            out << "                auto pSet = reinterpret_cast<long(__stdcall*)(void*, unsigned long, const void*, unsigned long)>(GetProcAddress(dwm, \"DwmSetWindowAttribute\"));\n";
            out << "                void* hwnd = reinterpret_cast<void*>(AppWindow().Id().Value);\n";
            out << "                if (!hwnd) hwnd = reinterpret_cast<void*>(::Windows::Win32::UI::WindowsAndMessaging::GetActiveWindow());\n";
            out << "                if (pSet && hwnd) pSet(hwnd, 20, &dark, sizeof(dark));\n";
            out << "                (void)FreeLibrary(dwm);\n";
            out << "            }\n";
            out << "        }\n";
        }
        if (!view.app_icon_path.empty())
            out << "        AppWindow().SetIcon(L\"" << view.app_icon_path << "\");\n";
        // Titlebar text — global for the app; supports UTF-8 emojis via wstring conversion
        if (!view.titlebar_text.empty())
        {
            out << "        // Set window title (UTF-8 emoji-safe via wstring)\n";
            out << "        WINUIC_UI_STEP(\"ui:set-title\");\n";
            out << "        auto wtitle = L\"" << view.titlebar_text << "\";\n";
            out << "        Title(wtitle);\n";
            out << "        WINUIC_UI_STEP(\"ui:set-title-done\");\n";
        }
        // Dark titlebar without extended content: theme the system titlebar itself.
        // (Handled above; nothing to do here when extend_content_into_titlebar is set.)
    }
    // Backdrop and dark mode directives (Window-only)
    if (is_window_class) {
        if (!effective_backdrop.empty())
        {
            out << "        WINUIC_UI_STEP(\"ui:backdrop-begin\");\n";
            if (effective_backdrop == "mica")
                out << "        { auto* sb = new Microsoft::UI::Xaml::Media::MicaBackdrop(); SystemBackdrop(Microsoft::UI::Xaml::Media::SystemBackdrop::wrap_borrowed(sb->m_unknown.get())); } // app-lifetime, intentionally leaked\n";
            else if (effective_backdrop == "acrylic")
                out << "        { auto* sb = new Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop(); SystemBackdrop(Microsoft::UI::Xaml::Media::SystemBackdrop::wrap_borrowed(sb->m_unknown.get())); } // app-lifetime, intentionally leaked\n";
            out << "        WINUIC_UI_STEP(\"ui:backdrop-done\");\n";
        }
        else
        {
            // Fluent 3D default: Mica gives every window the signature
            // WinUI 3 depth/lighting even when the source app declared no
            // backdrop; the merged XamlControlsResources supplies the real
            // Fluent control templates (rounded corners, acrylic flyouts,
            // hover/press depth states).
            out << "        { auto* sb = new Microsoft::UI::Xaml::Media::MicaBackdrop(); SystemBackdrop(Microsoft::UI::Xaml::Media::SystemBackdrop::wrap_borrowed(sb->m_unknown.get())); } // app-lifetime, intentionally leaked\n";
        }
        // Dark context menu follows dark mode — sync DWM and uxtheme
        out << "        WINUIC_UI_STEP(\"ui:sync-window\");\n";
        out << "        sync_window_theme(!is_current_light_theme() ? 1 : 0);\n";
        out << "        WINUIC_UI_STEP(\"ui:prologue-done\");\n";
    }

    if (!view.extend_content_into_titlebar && view.app_icon_path.empty() && !view.dark_titlebar && effective_backdrop.empty() && view.titlebar_text.empty())
        out << "        // no window-level directives\n";

    std::set<std::string> sample_source_vars = catalog_source_vars;
    sample_source_vars.insert(recently_visited_source_vars.begin(), recently_visited_source_vars.end());
    sample_source_vars.insert(all_controls_source_vars.begin(), all_controls_source_vars.end());
    if (!recently_visited_source_vars.empty())
        out << "        load_recent_history();\n";
    for (auto const& source_name : sample_source_vars)
    {
        auto state = std::find_if(view.state_vars.begin(), view.state_vars.end(),
            [&](layout_state_var const& candidate) { return candidate.name == source_name; });
        if (state == view.state_vars.end()) continue;
        std::string cap_name = state->name;
        if (!cap_name.empty()) cap_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap_name[0])));
        std::string member_name = (cap_name == state->name && !element_named_types.count(state->name))
            ? ("m_" + state->name) : state->name;
        out << "        {\n";
        out << "            void* sample_source_raw = nullptr;\n";
        out << "            winrt::check_hresult(winrt::IUnknown::qi_raw(" << member_name
            << ".get_abi(), winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::iid(), &sample_source_raw));\n";
        out << "            auto sample_source_vector = winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::from_abi(sample_source_raw);\n";
        out << "            if (sample_source_vector.get_Size() == 0) {\n";
        out << "                auto __sample_catalog = get_default_sample_catalog();\n";
        if (recently_visited_source_vars.count(source_name)) {
            out << "                for (auto const& __recent_id : get_recent_history_state().recently_visited_ids) {\n";
            out << "                    auto __cached_item = std::find_if(get_recent_history_state().recently_visited_items.begin(), get_recent_history_state().recently_visited_items.end(), [&](auto const& item) { return item.id == __recent_id; });\n";
            out << "                    if (__cached_item != get_recent_history_state().recently_visited_items.end() && !__cached_item->title.empty()) { sample_source_vector.Append(make_recent_history_item(*__cached_item)); continue; }\n";
            out << "                    for (std::uint32_t __sample_index = 0; __sample_index < __sample_catalog.get_Size(); ++__sample_index) {\n";
            out << "                        auto __sample = __sample_catalog.GetAt(__sample_index);\n";
            out << "                        void* __sample_map_raw = nullptr;\n";
            out << "                        winrt::check_hresult(winrt::IUnknown::qi_raw(__sample.get_abi(), winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::IInspectable>::iid(), &__sample_map_raw));\n";
            out << "                        auto __sample_uid = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(__sample_map_raw, L\"UniqueId\"), L\"\");\n";
            out << "                        auto* __sample_map_vt = *reinterpret_cast<void* const**>(__sample_map_raw);\n";
            out << "                        using __release_t = std::uint32_t(__stdcall*)(void*);\n";
            out << "                        reinterpret_cast<__release_t>(__sample_map_vt[2])(__sample_map_raw);\n";
            out << "                        if (__sample_uid.to_view() == __recent_id) { sample_source_vector.Append(__sample); break; }\n";
            out << "                    }\n";
            out << "                }\n";
        } else if (all_controls_source_vars.count(source_name)) {
            out << "                std::vector<winrt::IInspectable> __sorted_items;\n";
            out << "                auto __cat_iid = winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::IInspectable>::iid();\n";
            out << "                for (std::uint32_t __sample_index = 0; __sample_index < __sample_catalog.get_Size(); ++__sample_index) {\n";
            out << "                    __sorted_items.push_back(__sample_catalog.GetAt(__sample_index));\n";
            out << "                }\n";
            out << "                std::sort(__sorted_items.begin(), __sorted_items.end(), [&](winrt::IInspectable const& a, winrt::IInspectable const& b) {\n";
            out << "                    void* raw_a = nullptr; void* raw_b = nullptr;\n";
            out << "                    winrt::IUnknown::qi_raw(a.get_abi(), __cat_iid, &raw_a);\n";
            out << "                    winrt::IUnknown::qi_raw(b.get_abi(), __cat_iid, &raw_b);\n";
            out << "                    winrt::hstring title_a = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(raw_a, L\"Title\"), L\"\");\n";
            out << "                    winrt::hstring title_b = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(raw_b, L\"Title\"), L\"\");\n";
            out << "                    if (raw_a) { auto* vt = *reinterpret_cast<void* const**>(raw_a); reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vt[2])(raw_a); }\n";
            out << "                    if (raw_b) { auto* vt = *reinterpret_cast<void* const**>(raw_b); reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vt[2])(raw_b); }\n";
            out << "                    return title_a.to_view() < title_b.to_view();\n";
            out << "                });\n";
            out << "                for (std::size_t __si = 0; __si < __sorted_items.size(); ++__si) sample_source_vector.Append(__sorted_items[__si]);\n";
        } else {
            out << "                for (std::uint32_t __sample_index = 0; __sample_index < __sample_catalog.get_Size(); ++__sample_index) {\n";
            out << "                    auto __sample = __sample_catalog.GetAt(__sample_index);\n";
            out << "                    void* __sample_map_raw = nullptr;\n";
            out << "                    winrt::check_hresult(winrt::IUnknown::qi_raw(__sample.get_abi(), winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::IInspectable>::iid(), &__sample_map_raw));\n";
            out << "                    bool __is_new = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(__sample_map_raw, L\"IsNew\"), false);\n";
            out << "                    bool __is_updated = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(__sample_map_raw, L\"IsUpdated\"), false);\n";
            out << "                    auto* __sample_map_vt = *reinterpret_cast<void* const**>(__sample_map_raw);\n";
            out << "                    using __release_t = std::uint32_t(__stdcall*)(void*);\n";
            out << "                    reinterpret_cast<__release_t>(__sample_map_vt[2])(__sample_map_raw);\n";
            out << "                    if (__is_new || __is_updated) sample_source_vector.Append(__sample);\n";
            out << "                }\n";
        }
        out << "            }\n";
        out << "        }\n";
    }

    out << "\n        // build the UI tree\n";


    // Control emission engine and recursive tree traversal
#include "winuic_control.ixx"

    // Emit root nodes — MicaBackdrop becomes SystemBackdrop(); Flyouts register into Resources
    std::vector<std::string> root_vars;
    for (auto const& rn : view.root)
    {
        if (rn.control == "MicaBackdrop") {
            if (effective_backdrop.empty()) effective_backdrop = "mica";
            continue;
        }
        std::string rv = emit_node(emit_node, rn, 0, {});
        if (rv.empty()) continue;
        if (rn.control.ends_with("Flyout") || rn.control == "Flyout")
        {
            std::string res_key = rn.control;
            for (auto const& [k, v] : rn.props) {
                if (k == "name" || k == "Name" || k == "x:Name") { res_key = v; break; }
            }
            for (auto const& [k, v] : rn.raw_attrs) {
                if (k == "name" || k == "Name" || k == "x:Name") { res_key = v; break; }
            }
            out << "        insert_resource(Resources(), L\"" << res_key << "\", " << rv << ");\n";
        }
        else
        {
            root_vars.push_back(std::move(rv));
        }
    }

    // Set the single root child as Content() (Window: the window content;
    // Page/UserControl: the page's visual root - both must attach or the
    // view renders empty). Templated ContentControls (OpacityMaskView, Button)
    // use Content for user payload, NOT visual root; setting root as Content
    // creates a cycle with PART_ContentPresenter.
    bool is_content_control_base = (view.base_type == "ContentControl" || view.base_type == "Button" || view.base_type == "ToggleButton")
        && view.base_type != "UserControl" && view.base_type != "Page" && !view.base_type.ends_with("Page");
    if (root_vars.size() == 1)
    {
        if (!is_content_control_base)
        {
            out << "        Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << root_vars[0] << ".m_unknown.get()));\n";
        }
        if (is_window_class)
        {
            out << "        g_theme_state.window_id = AppWindow().Id().Value;\n";
            out << "        if (!g_theme_state.main_hwnd) g_theme_state.main_hwnd = reinterpret_cast<void*>(static_cast<std::uintptr_t>(AppWindow().Id().Value));\n";
            if (view.extend_content_into_titlebar)
                out << "        g_theme_state.is_extended_titlebar = true;\n";
            out << "        g_theme_state.root_element = static_cast<void*>(" << root_vars[0] << ".m_unknown.get());\n";
            out << "        apply_app_theme(!is_current_light_theme());\n";
        }
        // Drag region + caption theming AFTER the tree is built: the
        // named title-bar element must be assigned before SetTitleBar takes it
        // (a null element here would kill all titlebar dragging).
        if (is_window_class && view.extend_content_into_titlebar && !title_bar_name.empty())
        {
            out << "        SetTitleBar(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << title_bar_name << ".m_unknown.get()));\n";
            out << "        g_theme_state.window_id = AppWindow().Id().Value;\n";
            out << "        sync_titlebar_theme(!is_current_light_theme());\n";
            out << "        sync_window_theme(!is_current_light_theme() ? 1 : 0);\n";
        }
    }
    else if (root_vars.size() > 1)
    {
        // Multiple root children — wrap in a Grid
        out << "        auto _root = Grid();\n";
        for (auto const& rv : root_vars)
            out << "        _root.Children().Append(" << rv << ");\n";
        if (!is_content_control_base)
        {
            out << "        Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(_root.m_unknown.get()));\n";
        }
        if (is_window_class)
        {
            out << "        g_theme_state.window_id = AppWindow().Id().Value;\n";
            out << "        if (!g_theme_state.main_hwnd) g_theme_state.main_hwnd = reinterpret_cast<void*>(static_cast<std::uintptr_t>(AppWindow().Id().Value));\n";
            if (view.extend_content_into_titlebar)
                out << "        g_theme_state.is_extended_titlebar = true;\n";
            out << "        g_theme_state.root_element = static_cast<void*>(_root.m_unknown.get());\n";
            out << "        apply_app_theme(!is_current_light_theme());\n";
        }
        if (is_window_class && view.extend_content_into_titlebar && !title_bar_name.empty())
        {
            out << "        SetTitleBar(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << title_bar_name << ".m_unknown.get()));\n";
            out << "        g_theme_state.window_id = AppWindow().Id().Value;\n";
            out << "        sync_titlebar_theme(!is_current_light_theme());\n";
            out << "        sync_window_theme(!is_current_light_theme() ? 1 : 0);\n";
        }
    }

    {
        if (is_nav_window)
        {
            bool has_loaded_cb = false;
            for (auto const& cb : callbacks)
                if (cb.find("Loaded") != std::string::npos) { has_loaded_cb = true; break; }
            if (!has_loaded_cb)
                out << "        " << nav_view_name << ".add_Loaded([this](auto const&, auto const&) { _initialize_navigation(); });\n";
            for (auto const& [tag, page_name] : view.nav_pages)
            {
                auto named_item = named_elements.find(tag);
                auto state_item = element_named_types.find(tag);
                auto item_type = named_item != named_elements.end()
                    ? &named_item->second
                    : state_item != element_named_types.end() ? &state_item->second : nullptr;
                if (!item_type || item_type->find("NavigationViewItem") == std::string::npos)
                    continue;
                out << "        if (!" << tag << ".Tag().get_abi()) " << tag
                    << ".Tag(winrt::box_value(winrt::hstring(L\"" << tag << "\")));\n";
            }
        }
    }

    out << "        for (auto const& [__elem, __get_target] : _deferred_labeled_by) {\n";
    out << "            auto __target_elem = __get_target();\n";
    out << "            if (__elem.m_unknown.get() && __target_elem.m_unknown.get())\n";
    out << "                AutomationProperties::SetLabeledBy(__elem, __target_elem);\n";
    out << "        }\n";
    out << "        for (auto const& __rp_fn : _deferred_relative_panel) __rp_fn();\n";
    if (view.name == "SettingsPage")
    {
        if (has_elem("themeMode"))
            out << "        if (themeMode.m_unknown.get()) themeMode.SelectedIndex(g_theme_state.theme_idx == 1 ? 0 : (g_theme_state.theme_idx == 2 ? 1 : 2));\n";
        if (has_elem("navigationLocation"))
            out << "        if (navigationLocation.m_unknown.get()) navigationLocation.SelectedIndex(0);\n";
    }
    if (view.name == "HorizontalScrollContainer")
    {
        out << "        if (ContentGrid.m_unknown.get()) {\n";
        out << "            ContentGrid.add_SizeChanged([this](auto const&, auto const&) { UpdateScrollButtons(); });\n";
        out << "        }\n";
        out << "        if (scroller.m_unknown.get()) {\n";
        out << "            scroller.add_Loaded([this](auto const&, auto const&) { UpdateScrollButtons(); });\n";
        out << "        }\n";
        out << "        add_Loaded([this](auto const&, auto const&) { UpdateScrollButtons(); });\n";
        out << "        m_bindings.push_back([this]() { UpdateScrollButtons(); });\n";
    }
    if (!recently_visited_source_vars.empty() && !recent_section_element_names.empty())
    {
        out << "        bool __has_recent_items = false;\n";
        for (auto const& source_name : recently_visited_source_vars)
        {
            std::string cap_name = source_name;
            if (!cap_name.empty()) cap_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap_name[0])));
            std::string member_name = (cap_name == source_name && !element_named_types.count(source_name))
                ? ("m_" + source_name) : source_name;
            out << "        { void* __recent_raw = nullptr; if (winrt::IUnknown::qi_raw(" << member_name
                << ".get_abi(), winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::iid(), &__recent_raw) >= 0 && __recent_raw) {\n";
            out << "            auto __recent_vector = winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::from_abi(__recent_raw);\n";
            out << "            __has_recent_items = __has_recent_items || __recent_vector.get_Size() != 0;\n";
            out << "        } }\n";
        }
        for (auto const& element_name : recent_section_element_names)
            if (has_elem(element_name))
                out << "        if (" << element_name << ".m_unknown.get()) " << element_name
                    << ".Visibility(__has_recent_items ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n";
    }
    if (is_nav_window)
    {
        out << "        _init_nav_helpers();\n";
    }
    out << "    }\n";

    // Emit template factory functions (no load_xaml)
    for (auto const& tfr : pending_factory_funcs)
    {
        out << tfr.func_body;
    }
    } // end if (!is_app_class) — BuildUI() only for Window/UserControl

    std::string loaded_cb_name;
    if (is_nav_window)
    {
        for (auto const& cbn : callbacks) {
            if (cbn.find("NavigationView") != std::string::npos && cbn.find("Loaded") != std::string::npos) {
                loaded_cb_name = cbn;
                break;
            }
        }
        if (loaded_cb_name.empty()) {
            for (auto const& cbn : callbacks)
                if (cbn.find("Loaded") != std::string::npos) { loaded_cb_name = cbn; break; }
        }
        out << "    inline static " << view.name << "View* s_this_nav = nullptr;\n";
        out << "    void _init_nav_helpers() {\n";
        out << "        s_this_nav = this;\n";
        out << "        static void* s_nav_ctrl = nullptr;\n";
        out << "        static void* s_titlebar_ctrl = nullptr;\n";
        out << "        s_nav_ctrl = " << nav_view_name << ".m_unknown.get();\n";
        if (!title_bar_name.empty())
        {
            out << "        if (" << title_bar_name << ".m_unknown.get()) s_titlebar_ctrl = " << title_bar_name << ".m_unknown.get();\n";
        }
        out << "        g_theme_state.navigate_to = [](const wchar_t* t) {\n";
        out << "            if (s_this_nav && t) s_this_nav->_navigate_to_tag(t);\n";
        out << "        };\n";
        out << "        g_theme_state.set_nav_mode = [](int mode) {\n";
        out << "            auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();\n";
        out << "            auto apply_mode = [mode]() {\n";
        out << "                if (s_nav_ctrl) {\n";
        out << "                    auto nv = winrt::Microsoft::UI::Xaml::Controls::NavigationView::wrap_borrowed(s_nav_ctrl);\n";
        out << "                    nv.PaneDisplayMode(mode == 1 ? winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::Top : winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::Auto);\n";
        out << "                }\n";
        out << "                if (s_titlebar_ctrl) {\n";
        out << "                    auto tb = winrt::Microsoft::UI::Xaml::Controls::TitleBar::wrap_borrowed(s_titlebar_ctrl);\n";
        out << "                    tb.IsPaneToggleButtonVisible(mode != 1);\n";
        out << "                }\n";
        out << "            };\n";
        out << "            if (dq.get_abi()) dq.TryEnqueue(apply_mode);\n";
        out << "            else apply_mode();\n";
        out << "        };\n";
        out << "    }\n";
        out << "\n    struct _nav_history_entry { std::wstring tag; winrt::IInspectable page; winrt::IInspectable content; };\n";
        out << "    struct _nav_selection_scope { bool& active; explicit _nav_selection_scope(bool& value) : active(value) { active = true; } ~_nav_selection_scope() { active = false; } };\n";
        out << "    std::vector<_nav_history_entry> m_nav_history;\n";
        out << "    std::map<std::wstring, _nav_history_entry> m_nav_cache;\n";
        out << "    std::wstring m_current_nav_tag;\n";
        out << "    winrt::IInspectable m_current_nav_page;\n";
        out << "    bool m_syncing_nav_selection = false;\n";
        out << "        bool m_navigation_ready = false;\n";
        out << "\n    winrt::hstring _navigation_item_tag(winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem const& item)\n    {\n";
        out << "        auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), winrt::hstring{});\n";
        out << "        if (tag.empty()) tag = item.Name();\n";
        out << "        if (tag.empty()) tag = winrt::unbox_value_or<winrt::hstring>(item.Content(), winrt::hstring{});\n";
        out << "        return tag;\n";
        out << "    }\n";
        out << "\n    void _sync_navigation_selection(std::wstring_view tag)\n    {\n";
        out << "        auto find_item = [&](auto&& self, auto const& items) -> winrt::IInspectable {\n";
        out << "            if (!items) return {};\n";
        out << "            for (std::uint32_t i = 0; i < items.get_Size(); ++i) {\n";
        out << "                auto candidate = items.GetAt(i);\n";
        out << "                void* item_out = nullptr;\n";
        out << "                if (!candidate || winrt::impl::query_interface(candidate.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem>, &item_out) < 0 || !item_out) continue;\n";
        out << "                auto item = winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem::from_abi(item_out);\n";
        out << "                auto item_tag = _navigation_item_tag(item);\n";
        out << "                if (!item_tag.empty() && item_tag.to_view() == tag) return candidate;\n";
        out << "                if (auto child = self(self, item.MenuItems())) return child;\n";
        out << "            }\n";
        out << "            return {};\n";
        out << "        };\n";
        out << "        auto selected = find_item(find_item, " << nav_view_name << ".MenuItems());\n";
        out << "        if (selected) {\n";
        out << "            _nav_selection_scope scope(m_syncing_nav_selection);\n";
        out << "            " << nav_view_name << ".SelectedItem(selected);\n";
        out << "        }\n";
        out << "    }\n";
        out << "\n    void _navigate_to_tag(std::wstring_view tag, bool record_history = true, winrt::IInspectable cached_page = {}, winrt::IInspectable cached_content = {})\n    {\n";
        out << "        if (!m_navigation_ready || m_syncing_nav_selection || (tag == m_current_nav_tag && m_current_nav_page)) return;\n";
        out << "        _init_nav_helpers();\n";
        out << "        winrt::IInspectable (*factory)() = nullptr;\n";
        for (auto const& [tag, page_name] : view.nav_pages)
        {
            std::string fn = "create_";
            std::string ns_part = page_name;
            auto d = page_name.rfind('.');
            std::string page_class = (d != std::string::npos) ? page_name.substr(d + 1) : page_name;
            fn += page_class;
            out << "        if (tag == L\"" << tag << "\") factory = " << fn << ";\n";
        }
        out << "        if (!factory && !cached_content) return;\n";
        out << "        try\n";
        out << "        {\n";
        out << "            auto __navigation_started = std::chrono::steady_clock::now();\n";
        out << "            if (!cached_page && !cached_content) {\n";
        out << "                auto __cached = m_nav_cache.find(std::wstring(tag));\n";
        out << "                if (__cached != m_nav_cache.end()) { cached_page = __cached->second.page; cached_content = __cached->second.content; }\n";
        out << "            }\n";
        out << "            winrt::IInspectable page = cached_page;\n";
        out << "            auto __page_build_started = std::chrono::steady_clock::now();\n";
        out << "            if (!page) page = factory();\n";
        out << "            auto __page_build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - __page_build_started).count();\n";
        out << "            boot_log((std::string(\"navigate:factory-ms:\") + std::to_string(__page_build_ms)).c_str());\n";
        out << "            if (page)\n";
        out << "            {\n";
        out << "                void* fe_raw = nullptr;\n";
        out << "                if (page.get_abi() && winrt::IUnknown::qi_raw(page.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::IFrameworkElement>, &fe_raw) >= 0 && fe_raw)\n";
        out << "                {\n";
        out << "                    winrt::Microsoft::UI::Xaml::IFrameworkElement fe{fe_raw};\n";
        out << "                    fe.put_RequestedTheme(get_global_theme());\n";
        out << "                    fe.put_HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);\n";
        out << "                }\n";
        out << "                auto previous_content = " << frame_name << ".Content();\n";
        out << "                if (cached_content)\n";
        out << "                {\n";
        out << "                    " << frame_name << ".Content(cached_content);\n";
        out << "                }\n";
        out << "                else\n";
        out << "                {\n";
        out << "                    if (tag != L\"Home\" && tag != L\"HomePage\")\n";
        out << "                    {\n";
        out << "                        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer sv{};\n";
        out << "                        sv.VerticalScrollBarVisibility(winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Auto);\n";
        out << "                        sv.VerticalScrollMode(winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Enabled);\n";
        out << "                        sv.HorizontalScrollBarVisibility(winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled);\n";
        out << "                        sv.HorizontalScrollMode(winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Disabled);\n";
        out << "                        sv.Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(page.get_abi()));\n";
        out << "                        " << frame_name << ".Content(sv);\n";
        out << "                    }\n";
        out << "                    else\n";
        out << "                    {\n";
        out << "                        " << frame_name << ".Content(page);\n";
        out << "                    }\n";
        out << "                }\n";
        out << "                if (record_history && !m_current_nav_tag.empty() && m_current_nav_tag != tag && previous_content && m_current_nav_page)\n";
        out << "                    m_nav_history.push_back(_nav_history_entry{m_current_nav_tag, m_current_nav_page, previous_content});\n";
        out << "                if (!m_current_nav_tag.empty() && m_current_nav_page && previous_content)\n";
        out << "                    m_nav_cache[m_current_nav_tag] = _nav_history_entry{m_current_nav_tag, m_current_nav_page, previous_content};\n";
        out << "                m_current_nav_tag = std::wstring(tag);\n";
        out << "                m_current_nav_page = page;\n";
        out << "                m_nav_cache[m_current_nav_tag] = _nav_history_entry{m_current_nav_tag, page, " << frame_name << ".Content()};\n";
        out << "                if (tag != L\"Home\" && tag != L\"HomePage\" && tag != L\"AllControls\" && tag != L\"AllControlsPage\" && tag != L\"Settings\" && tag != L\"__settings__\") {\n";
        out << "                    load_recent_history();\n";
        out << "                    auto __cat = get_default_sample_catalog();\n";
        out << "                    auto __cat_iid = winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::IInspectable>::iid();\n";
        out << "                    for (std::uint32_t __si = 0; __si < __cat.get_Size(); ++__si) {\n";
        out << "                        auto __s = __cat.GetAt(__si);\n";
        out << "                        void* __s_raw = nullptr;\n";
        out << "                        if (winrt::IUnknown::qi_raw(__s.get_abi(), __cat_iid, &__s_raw) >= 0 && __s_raw) {\n";
        out << "                            auto __suid = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(__s_raw, L\"UniqueId\"), L\"\");\n";
        out << "                            if (__suid == tag || (tag.ends_with(L\"Page\") && __suid == tag.substr(0, tag.size() - 4))) {\n";
        out << "                                auto __stitle = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(__s_raw, L\"Title\"), L\"\");\n";
        out << "                                auto __ssub = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(__s_raw, L\"Subtitle\"), L\"\");\n";
        out << "                                auto __simg = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(__s_raw, L\"ImagePath\"), L\"\");\n";
        out << "                                bool __snew = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(__s_raw, L\"IsNew\"), false);\n";
        out << "                                bool __supd = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(__s_raw, L\"IsUpdated\"), false);\n";
        out << "                                bool __sexp = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(__s_raw, L\"IsExperimental\"), false);\n";
        out << "                                auto* __vt = *reinterpret_cast<void* const**>(__s_raw);\n";
        out << "                                reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(__vt[2])(__s_raw);\n";
        out << "                                std::wstring __actual_id(__suid.to_view());\n";
        out << "                                auto& __r_ids = get_recent_history_state().recently_visited_ids;\n";
        out << "                                for (auto __it = __r_ids.begin(); __it != __r_ids.end();) {\n";
        out << "                                    if (*__it == __actual_id) __it = __r_ids.erase(__it); else ++__it;\n";
        out << "                                }\n";
        out << "                                __r_ids.insert(__r_ids.begin(), __actual_id);\n";
        out << "                                auto& __r_items = get_recent_history_state().recently_visited_items;\n";
        out << "                                for (auto __it = __r_items.begin(); __it != __r_items.end();) {\n";
        out << "                                    if (__it->id == __actual_id) __it = __r_items.erase(__it); else ++__it;\n";
        out << "                                }\n";
        out << "                                RecentItemData __rec;\n";
        out << "                                __rec.id = __actual_id; __rec.title = __stitle.c_str();\n";
        out << "                                __rec.subtitle = __ssub.c_str(); __rec.image_path = __simg.c_str();\n";
        out << "                                __rec.is_new = __snew; __rec.is_updated = __supd; __rec.is_experimental = __sexp;\n";
        out << "                                __r_items.insert(__r_items.begin(), std::move(__rec));\n";
        out << "                                save_recent_history();\n";
        out << "                                break;\n";
        out << "                            }\n";
        out << "                            auto* __vt = *reinterpret_cast<void* const**>(__s_raw);\n";
        out << "                            reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(__vt[2])(__s_raw);\n";
        out << "                        }\n";
        out << "                    }\n";
        out << "                }\n";
        out << "                else if (tag == L\"Home\" || tag == L\"HomePage\") {\n";
        out << "                    get_HomePage()._refresh_recent_items();\n";
        out << "                }\n";
        out << "                auto __navigation_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - __navigation_started).count();\n";
        out << "                boot_log((std::string(\"navigate:complete-ms:\") + std::to_string(__navigation_ms)).c_str());\n";
        if (!title_bar_name.empty())
        {
            out << "                if (" << title_bar_name << ".m_unknown.get()) {\n";
            out << "                    " << title_bar_name << ".IsBackButtonVisible(!m_nav_history.empty());\n";
            out << "                    " << title_bar_name << ".IsBackButtonEnabled(!m_nav_history.empty());\n";
            out << "                }\n";
        }
        out << "                _sync_navigation_selection(tag);\n";
        out << "            }\n";
        out << "        }\n";
        out << "        catch (...) { winrt::report_current_exception(\"navigate\"); boot_log(\"navigate:catch\"); }\n";
        out << "    }\n";
        out << "    void _initialize_navigation()\n    {\n";
        out << "        m_navigation_ready = true;\n";
        out << "        if (" << frame_name << ".Content() != nullptr) return;\n";
        out << "        auto items = " << nav_view_name << ".MenuItems();\n";
        out << "        if (!items || items.get_Size() == 0) return;\n";
        out << "        auto first = items.GetAt(0);\n";
        out << "        if (!first) return;\n";
        out << "        void* item_out = nullptr;\n";
        out << "        if (winrt::impl::query_interface(first.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem>, &item_out) >= 0 && item_out)\n";
        out << "        {\n";
        out << "            auto item = winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem::from_abi(item_out);\n";
        out << "            auto tag = _navigation_item_tag(item);\n";
        out << "            if (!tag.empty()) _navigate_to_tag(tag.to_view());\n";
        out << "        }\n";
        out << "    }\n";
        out << "    void _go_back()\n    {\n";
        out << "        if (m_nav_history.empty()) return;\n";
        out << "        auto previous = m_nav_history.back();\n";
        out << "        m_nav_history.pop_back();\n";
        out << "        _navigate_to_tag(previous.tag, false, previous.page, previous.content);\n";
        out << "    }\n";
    }

    if (!recently_visited_source_vars.empty())
    {
        out << "\n    void _refresh_recent_items()\n    {\n";
        out << "        auto __sample_catalog = get_default_sample_catalog();\n";
        out << "        auto __iid_map = winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::IInspectable>::iid();\n";
        std::size_t recent_index = 0;
        for (auto const& source_name : recently_visited_source_vars)
        {
            std::string cap_name = source_name;
            if (!cap_name.empty()) cap_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap_name[0])));
            std::string member_name = (cap_name == source_name && !element_named_types.count(source_name))
                ? ("m_" + source_name) : source_name;
            auto suffix = std::to_string(recent_index++);
            out << "        void* __recent_raw_" << suffix << " = nullptr;\n";
            out << "        winrt::check_hresult(winrt::IUnknown::qi_raw(" << member_name
                << ".get_abi(), winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::iid(), &__recent_raw_" << suffix << "));\n";
            out << "        auto __recent_vector_" << suffix << " = winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::from_abi(__recent_raw_" << suffix << ");\n";
            out << "        __recent_vector_" << suffix << ".Clear();\n";
            out << "        for (auto const& __recent_id : get_recent_history_state().recently_visited_ids) {\n";
            out << "            auto __cached_item = std::find_if(get_recent_history_state().recently_visited_items.begin(), get_recent_history_state().recently_visited_items.end(), [&](auto const& item) { return item.id == __recent_id; });\n";
            out << "            if (__cached_item != get_recent_history_state().recently_visited_items.end() && !__cached_item->title.empty()) { __recent_vector_" << suffix << ".Append(make_recent_history_item(*__cached_item)); continue; }\n";
            out << "            for (std::uint32_t __sample_index = 0; __sample_index < __sample_catalog.get_Size(); ++__sample_index) {\n";
            out << "                auto __sample = __sample_catalog.GetAt(__sample_index);\n";
            out << "                void* __sample_map_raw = nullptr;\n";
            out << "                winrt::check_hresult(winrt::IUnknown::qi_raw(__sample.get_abi(), __iid_map, &__sample_map_raw));\n";
            out << "                auto __sample_uid = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(__sample_map_raw, L\"UniqueId\"), L\"\");\n";
            out << "                auto* __sample_map_vt = *reinterpret_cast<void* const**>(__sample_map_raw);\n";
            out << "                using __release_t = std::uint32_t(__stdcall*)(void*);\n";
            out << "                reinterpret_cast<__release_t>(__sample_map_vt[2])(__sample_map_raw);\n";
            out << "                if (__sample_uid.to_view() == __recent_id) { __recent_vector_" << suffix << ".Append(__sample); break; }\n";
            out << "            }\n";
            out << "        }\n";
        }
        out << "        bool __has_recent_items = false;\n";
        for (auto const& source_name : recently_visited_source_vars)
        {
            std::string cap_name = source_name;
            if (!cap_name.empty()) cap_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap_name[0])));
            std::string member_name = (cap_name == source_name && !element_named_types.count(source_name))
                ? ("m_" + source_name) : source_name;
            out << "        { void* __recent_check_raw = nullptr; if (winrt::IUnknown::qi_raw(" << member_name
                << ".get_abi(), winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::iid(), &__recent_check_raw) >= 0 && __recent_check_raw) {\n";
            out << "            auto __recent_check_vector = winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::from_abi(__recent_check_raw);\n";
            out << "            __has_recent_items = __has_recent_items || __recent_check_vector.get_Size() != 0;\n";
            out << "        } }\n";
        }
        for (auto const& element_name : recent_section_element_names)
            if (has_elem(element_name))
                out << "        if (" << element_name << ".m_unknown.get()) " << element_name
                    << ".Visibility(__has_recent_items ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n";
        out << "    }\n";
    }

    // Emit event handler stubs INSIDE class body (avoids C2039 with imported type)
    std::set<std::string> extra_members; // bare C# members referenced by translated bodies
    for (auto const& cb : callbacks)
    {
        if (cb == "raw") continue;
        out << "\n    template <typename Sender, typename Args>\n";
        out << "    void " << cb << "(Sender const& sender, Args const& args)\n    {\n";
        if (!recently_visited_source_vars.empty()
            && (cb.find("Loaded") != std::string::npos || cb.find("loaded") != std::string::npos))
            out << "        _refresh_recent_items();\n";
        if (view.name == "SettingsPage" && (cb == "NavigationLocationSelectionChanged" || cb == "navigationLocation_SelectionChanged") && has_elem("navigationLocation"))
        {
            out << "        int idx = navigationLocation.SelectedIndex();\n";
            out << "        if (g_theme_state.set_nav_mode && idx >= 0) { g_theme_state.set_nav_mode(idx); return; }\n";
        }
        if (view.name == "HorizontalScrollContainer")
        {
            if (cb == "ScrollBackBtn_Click" || cb == "ScrollBackBtnClick")
            {
                out << "        if (scroller.m_unknown.get()) {\n";
                out << "            double step = (scroller.ViewportWidth() > 50.0) ? scroller.ViewportWidth() : 320.0;\n";
                out << "            double target = scroller.HorizontalOffset() - step;\n";
                out << "            if (target < 0.0) target = 0.0;\n";
                out << "            scroller.ChangeView(winrt::Windows::Foundation::IReference<double>(target), nullptr, nullptr);\n";
                out << "        }\n";
            }
            else if (cb == "ScrollForwardBtn_Click" || cb == "ScrollForwardBtnClick")
            {
                out << "        if (scroller.m_unknown.get()) {\n";
                out << "            double step = (scroller.ViewportWidth() > 50.0) ? scroller.ViewportWidth() : 320.0;\n";
                out << "            double target = scroller.HorizontalOffset() + step;\n";
                out << "            scroller.ChangeView(winrt::Windows::Foundation::IReference<double>(target), nullptr, nullptr);\n";
                out << "        }\n";
            }
            else if (cb == "Scroller_SizeChanged" || cb == "ScrollerSizeChanged")
            {
                out << "        UpdateScrollButtons();\n";
                out << "        return;\n";
                out << "    }\n";
                out << "    void UpdateScrollButtons()\n    {\n";
                out << "        auto update_btns = [this]() {\n";
                out << "            if (scroller.m_unknown.get()) {\n";
                out << "                if (ScrollBackBtn.m_unknown.get()) {\n";
                out << "                    double off = scroller.HorizontalOffset();\n";
                out << "                    auto b_vis = (off > 1.0) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;\n";
                out << "                    if (ScrollBackBtn.Visibility() != b_vis) ScrollBackBtn.Visibility(b_vis);\n";
                out << "                }\n";
                out << "                if (ScrollForwardBtn.m_unknown.get()) {\n";
                out << "                    bool can_scroll = (scroller.ScrollableWidth() > 0)\n";
                out << "                        || (scroller.ExtentWidth() > scroller.ViewportWidth() && scroller.ViewportWidth() > 0)\n";
                out << "                        || (ContentGrid.m_unknown.get() && scroller.ActualWidth() > 0 && ContentGrid.ActualWidth() > scroller.ActualWidth());\n";
                out << "                    if (!can_scroll && m_Source.get_abi()) {\n";
                out << "                        void* ic_raw = nullptr;\n";
                out << "                        if (winrt::IUnknown::qi_raw(m_Source.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::IItemsControl>, &ic_raw) >= 0 && ic_raw) {\n";
                out << "                            auto ic = winrt::Microsoft::UI::Xaml::Controls::ItemsControl::from_abi(ic_raw);\n";
                out << "                            if (ic.ItemsSource().get_abi()) {\n";
                out << "                                void* v_raw = nullptr;\n";
                out << "                                if (winrt::IUnknown::qi_raw(ic.ItemsSource().get_abi(), winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::iid(), &v_raw) >= 0 && v_raw) {\n";
                out << "                                    auto vec = winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::from_abi(v_raw);\n";
                out << "                                    if (vec.get_Size() >= 4) can_scroll = true;\n";
                out << "                                }\n";
                out << "                            }\n";
                out << "                        }\n";
                out << "                    }\n";
                out << "                    auto f_vis = can_scroll ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;\n";
                out << "                    if (ScrollForwardBtn.Visibility() != f_vis) ScrollForwardBtn.Visibility(f_vis);\n";
                out << "                }\n";
                out << "            }\n";
                out << "        };\n";
                out << "        update_btns();\n";
                out << "        auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();\n";
                out << "        if (dq.get_abi()) {\n";
                out << "            dq.TryEnqueue(update_btns);\n";
                out << "        }\n";
                out << "        return;\n";
            }
            else if (cb == "Scroller_ViewChanging" || cb == "ScrollerViewChanging" || cb == "Scroller_ViewChanged" || cb == "ScrollerViewChanged")
            {
                out << "        if (scroller.m_unknown.get()) {\n";
                out << "            double off = scroller.HorizontalOffset();\n";
                out << "            double sw = scroller.ScrollableWidth();\n";
                out << "            if (ScrollBackBtn.m_unknown.get()) {\n";
                out << "                auto b_vis = (off > 1.0) ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;\n";
                out << "                if (ScrollBackBtn.Visibility() != b_vis) ScrollBackBtn.Visibility(b_vis);\n";
                out << "            }\n";
                out << "            if (ScrollForwardBtn.m_unknown.get()) {\n";
                out << "                bool can_fwd = (sw > 1.0 && off < sw - 1.0)\n";
                out << "                    || (scroller.ExtentWidth() > scroller.ViewportWidth() && off < (scroller.ExtentWidth() - scroller.ViewportWidth() - 1.0));\n";
                out << "                auto f_vis = can_fwd ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;\n";
                out << "                if (ScrollForwardBtn.Visibility() != f_vis) ScrollForwardBtn.Visibility(f_vis);\n";
                out << "            }\n";
                out << "        }\n";
                out << "        return;\n";
            }
        }
        if (cb == "ClearRecentBtn_Click" || cb == "ClearRecentBtnClick")
        {
            out << "        load_recent_history();\n";
            out << "        get_recent_history_state().recently_visited_ids.clear();\n";
            out << "        get_recent_history_state().recently_visited_items.clear();\n";
            out << "        save_recent_history();\n";
        }
        else if (cb == "UnfavoriteBtn_Click" || cb == "UnfavoriteBtnClick")
        {
            out << "        load_recent_history();\n";
            out << "        get_recent_history_state().recently_visited_ids.clear();\n";
            out << "        get_recent_history_state().recently_visited_items.clear();\n";
            out << "        save_recent_history();\n";
        }
        // Nav synthesis: real behavior for the callbacks the reference apps
        // wire (SelectionChanged -> tag dispatch into the Frame; Loaded ->
        // select the first menu item so the first page navigates). These run
        // BEFORE the winmd-carried statements (which stay authoritative).
        if (is_nav_window)
        {
            if (cb.find("SelectionChanged") != std::string::npos)
            {
                out << "        if (!m_navigation_ready || m_syncing_nav_selection) return;\n";
                out << "        // synthesized: tag dispatch into " << frame_name << "\n";
                out << "        std::wstring __navigation_tag;\n";
                out << "        if (args.IsSettingsSelected()) __navigation_tag = L\"__settings__\";\n";
                out << "        else\n";
                out << "        {\n";
                out << "            auto selected = args.SelectedItem();\n";
                out << "            if (!selected) return;\n";
                out << "            void* item_out = nullptr;\n";
                out << "            if (winrt::impl::query_interface(selected.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem>, &item_out) < 0 || !item_out) return;\n";
                out << "            auto item = winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem::from_abi(item_out);\n";
                out << "            auto item_tag = _navigation_item_tag(item);\n";
                out << "            if (item_tag.empty()) return;\n";
                out << "            __navigation_tag = std::wstring(item_tag.to_view());\n";
                out << "        }\n";
                out << "        auto __dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();\n";
                out << "        if (!__dispatcher.get_abi() || !__dispatcher.TryEnqueue([this, __navigation_tag = std::move(__navigation_tag)] { _navigate_to_tag(__navigation_tag); }))\n";
                out << "        {\n";
                out << "            winrt::boot_log(\"navigation:dispatcher-rejected-selection\");\n";
                out << "        }\n";
            }
            if (!loaded_cb_name.empty() && cb == loaded_cb_name)
            {
                out << "        _initialize_navigation();\n";
            }
        }
        auto cb_it = view.callbacks.find(cb);
        if (!nav_view_name.empty() && (cb.find("PaneToggle") != std::string::npos || cb.find("panetoggle") != std::string::npos) && (cb_it == view.callbacks.end() || cb_it->second.empty()))
        {
            out << "        // synthesized: toggle navigation pane\n";
            out << "        " << nav_view_name << ".IsPaneOpen(!" << nav_view_name << ".IsPaneOpen());\n";
        }
        if (is_nav_window && (cb.find("BackRequested") != std::string::npos || cb.find("GoBack") != std::string::npos))
        {
            out << "        _go_back();\n";
        }
        if (is_nav_window && !search_box_name.empty() && (cb.ends_with("TextChanged") || cb.ends_with("QuerySubmitted")))
        {
            out << "        struct _nav_entry { wchar_t const* title; wchar_t const* tag; };\n";
            out << "        static constexpr _nav_entry s_entries[] = {\n";
            for (auto const& [tag, page_name] : view.nav_pages)
            {
                if (tag == "__settings__") continue;
                std::string title = tag;
                if (title.size() > 4 && title.ends_with("Page")) title = title.substr(0, title.size() - 4);
                out << "            { L\"" << escape_wstr(title) << "\", L\"" << escape_wstr(tag) << "\" },\n";
            }
            out << "            { L\"Settings\", L\"__settings__\" }\n";
            out << "        };\n";
            out << "        auto to_lo = [](std::wstring_view sv) { std::wstring r(sv); for (auto& ch : r) if (ch >= L'A' && ch <= L'Z') ch += (L'a' - L'A'); return r; };\n";
            if (cb.ends_with("TextChanged"))
            {
                out << "        if (args.get_abi()) {\n";
                out << "            void* tc_out = nullptr;\n";
                out << "            if (winrt::impl::query_interface(args.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs>, &tc_out) >= 0 && tc_out) {\n";
                out << "                auto tc = winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs::from_abi(tc_out);\n";
                out << "                if (tc.Reason() != winrt::Microsoft::UI::Xaml::Controls::AutoSuggestionBoxTextChangeReason::UserInput) return;\n";
                out << "            }\n";
                out << "        }\n";
                out << "        auto q = to_lo(" << search_box_name << ".Text().to_view());\n";
                out << "        if (q.empty()) { " << search_box_name << ".ItemsSource(winrt::IInspectable{nullptr}); return; }\n";
                out << "        auto vec = winrt::single_threaded_vector<winrt::IInspectable>();\n";
                out << "        int matches = 0;\n";
                out << "        for (auto const& e : s_entries) {\n";
                out << "            if (to_lo(e.title).starts_with(q) || to_lo(e.tag).starts_with(q)) {\n";
                out << "                inspectable_append(vec, winrt::box_value(winrt::hstring(e.title)));\n";
                out << "                if (++matches >= 12) break;\n";
                out << "            }\n";
                out << "        }\n";
                out << "        " << search_box_name << ".ItemsSource(vec);\n";
            }
            else
            {
                out << "        std::wstring q = std::wstring(" << search_box_name << ".Text().to_view());\n";
                out << "        if (args.get_abi()) {\n";
                out << "            void* qs_out = nullptr;\n";
                out << "            if (winrt::impl::query_interface(args.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxQuerySubmittedEventArgs>, &qs_out) >= 0 && qs_out) {\n";
                out << "                auto qs = winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxQuerySubmittedEventArgs::from_abi(qs_out);\n";
                out << "                auto chosen = winrt::unbox_value_or<winrt::hstring>(qs.ChosenSuggestion(), L\"\");\n";
                out << "                if (!chosen.empty()) q = std::wstring(chosen.to_view());\n";
                out << "                else if (!qs.QueryText().empty()) q = std::wstring(qs.QueryText().to_view());\n";
                out << "            }\n";
                out << "        }\n";
                out << "        auto ql = to_lo(q);\n";
                out << "        if (ql.empty()) return;\n";
                out << "        for (auto const& e : s_entries) {\n";
                out << "            if (to_lo(e.title) == ql || to_lo(e.tag) == ql) { _navigate_to_tag(e.tag); return; }\n";
                out << "        }\n";
                out << "        for (auto const& e : s_entries) {\n";
                out << "            if (to_lo(e.title).find(ql) != std::wstring::npos || to_lo(e.tag).find(ql) != std::wstring::npos) { _navigate_to_tag(e.tag); return; }\n";
                out << "        }\n";
            }
        }
        if (cb.ends_with("ContainerContentChanging"))
        {
            out << "        try {\n";
            out << "            void* e_raw = nullptr;\n";
            out << "            if (winrt::impl::query_interface(args.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::IContainerContentChangingEventArgs>, &e_raw) >= 0 && e_raw) {\n";
            out << "                auto e = winrt::Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs::from_abi(e_raw);\n";
            out << "                if (e.InRecycleQueue()) return;\n";
            out << "                auto container = e.ItemContainer();\n";
            out << "                if (!container.get_abi()) return;\n";
            out << "                auto root = container.ContentTemplateRoot();\n";
            out << "                if (!root.get_abi()) {\n";
            out << "                    if (e.Phase() < 1) e.RegisterUpdateCallback([this](auto const& s, auto const& next) { " << cb << "(s, next); });\n";
            out << "                    return;\n";
            out << "                }\n";
            out << "                auto item = e.Item();\n";
            out << "                if (item.get_abi()) {\n";
            out << "                    winrt::hstring title, subtitle, img_path;\n";
            out << "                    bool is_experimental = false;\n";
            out << "                    auto iid_map = winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::IInspectable>::iid();\n";
            out << "                    void* map_raw = nullptr;\n";
            out << "                    if (winrt::impl::query_interface(item.get_abi(), iid_map, &map_raw) >= 0 && map_raw) {\n";
            out << "                        auto get_val = [&](wchar_t const* k) -> winrt::hstring {\n";
            out << "                            auto v = theme_find_in_raw_hstring_map(map_raw, k);\n";
            out << "                            return winrt::unbox_value_or<winrt::hstring>(v, L\"\");\n";
            out << "                        };\n";
            out << "                        title = get_val(L\"Title\");\n";
            out << "                        subtitle = get_val(L\"Subtitle\");\n";
            out << "                        img_path = get_val(L\"ImagePath\");\n";
            out << "                        is_experimental = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(map_raw, L\"IsExperimental\"), false);\n";
            out << "                        auto* vt = *reinterpret_cast<void* const**>(map_raw);\n";
            out << "                        using release_t = std::uint32_t(__stdcall*)(void*);\n";
            out << "                        reinterpret_cast<release_t>(vt[2])(map_raw);\n";
            out << "                    }\n";
            out << "                    {\n";
            out << "                        bool title_set = false;\n";
            out << "                        bool sub_set = false;\n";
            out << "                        bool experimental_badge_found = false;\n";
            out << "                        std::function<void(winrt::Microsoft::UI::Xaml::UIElement const&)> bind_tree;\n";
            out << "                        bind_tree = [&](winrt::Microsoft::UI::Xaml::UIElement const& el) {\n";
            out << "                            if (!el.get_abi()) return;\n";
            out << "                            void* tb_raw = nullptr;\n";
            out << "                            if (winrt::impl::query_interface(el.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::ITextBlock>, &tb_raw) >= 0 && tb_raw) {\n";
            out << "                                auto tb = winrt::Microsoft::UI::Xaml::Controls::TextBlock::from_abi(tb_raw);\n";
            out << "                                if (tb.Text() == L\"Experimental\") {\n";
            out << "                                    experimental_badge_found = true;\n";
            out << "                                    return;\n";
            out << "                                }\n";
            out << "                                if (!title_set) {\n";
            out << "                                    tb.Text(title);\n";
            out << "                                    title_set = true;\n";
            out << "                                } else if (!sub_set) {\n";
            out << "                                    tb.Text(subtitle);\n";
            out << "                                    sub_set = true;\n";
            out << "                                }\n";
            out << "                                return;\n";
            out << "                            }\n";
            out << "                            void* im_raw = nullptr;\n";
            out << "                            if (winrt::impl::query_interface(el.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::IImage>, &im_raw) >= 0 && im_raw) {\n";
            out << "                                auto im = winrt::Microsoft::UI::Xaml::Controls::Image::from_abi(im_raw);\n";
            out << "                                if (!img_path.empty()) {\n";
            out << "                                    try {\n";
            out << "                                        auto isrc = image_source_lookup_th(img_path.c_str());\n";
            out << "                                        if (isrc.get_abi()) im.Source(isrc);\n";
            out << "                                        else im.Source(winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(img_path)));\n";
            out << "                                    } catch (...) { winrt::report_current_exception(\"card image binding\"); }\n";
            out << "                                }\n";
            out << "                                return;\n";
            out << "                            }\n";
            out << "                            void* p_raw = nullptr;\n";
            out << "                            void* border_raw = nullptr;\n";
            out << "                            if (winrt::impl::query_interface(el.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::IBorder>, &border_raw) >= 0 && border_raw) {\n";
            out << "                                auto border = winrt::Microsoft::UI::Xaml::Controls::Border::from_abi(border_raw);\n";
            out << "                                auto child = border.Child();\n";
            out << "                                void* label_raw = nullptr;\n";
            out << "                                if (child.get_abi() && winrt::impl::query_interface(child.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::ITextBlock>, &label_raw) >= 0 && label_raw) {\n";
            out << "                                    auto label = winrt::Microsoft::UI::Xaml::Controls::TextBlock::from_abi(label_raw);\n";
            out << "                                    if (label.Text() == L\"Experimental\") {\n";
            out << "                                        experimental_badge_found = true;\n";
            out << "                                        border.Visibility(is_experimental ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n";
            out << "                                        return;\n";
            out << "                                    }\n";
            out << "                                }\n";
            out << "                                bind_tree(child);\n";
            out << "                                return;\n";
            out << "                            }\n";
            out << "                            if (winrt::impl::query_interface(el.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::IPanel>, &p_raw) >= 0 && p_raw) {\n";
            out << "                                auto p = winrt::Microsoft::UI::Xaml::Controls::Panel::from_abi(p_raw);\n";
            out << "                                auto kids = p.Children();\n";
            out << "                                if (kids.get_abi()) {\n";
            out << "                                    uint32_t cnt = kids.Size();\n";
            out << "                                    for (uint32_t i = 0; i < cnt; ++i) bind_tree(kids.GetAt(i));\n";
            out << "                                }\n";
            out << "                            }\n";
            out << "                        };\n";
            out << "                        bind_tree(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(root.get_abi()));\n";
            out << "                        if (is_experimental && !experimental_badge_found) {\n";
            out << "                            void* panel_raw = nullptr;\n";
            out << "                            if (winrt::impl::query_interface(root.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::IPanel>, &panel_raw) >= 0 && panel_raw) {\n";
            out << "                                auto badge = winrt::Microsoft::UI::Xaml::Controls::Border();\n";
            out << "                                badge.Margin(winrt::Microsoft::UI::Xaml::Thickness{-8,-8,-8,-8});\n";
            out << "                                badge.Padding(winrt::Microsoft::UI::Xaml::Thickness{8,4,8,4});\n";
            out << "                                badge.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Bottom);\n";
            out << "                                badge.Background(theme_brush_th(L\"SystemFillColorCautionBackgroundBrush\"));\n";
            out << "                                badge.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{0,0,7,7});\n";
            out << "                                auto label = winrt::Microsoft::UI::Xaml::Controls::TextBlock();\n";
            out << "                                label.Text(L\"Experimental\");\n";
            out << "                                label.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center);\n";
            out << "                                label.Foreground(theme_brush_th(L\"SystemFillColorCautionBrush\"));\n";
            out << "                                label.FontSize(12);\n";
            out << "                                badge.Child(label);\n";
            out << "                                winrt::Microsoft::UI::Xaml::Controls::Grid::SetRow(badge, 1);\n";
            out << "                                winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumnSpan(badge, 2);\n";
            out << "                                auto panel = winrt::Microsoft::UI::Xaml::Controls::Panel::from_abi(panel_raw);\n";
            out << "                                panel.Children().Append(badge);\n";
            out << "                            }\n";
            out << "                        }\n";
            out << "                    }\n";
            out << "                }\n";
            out << "            }\n";
            out << "        } catch (...) { winrt::report_current_exception(\"container content changing\"); }\n";
        }
        if (cb.ends_with("ItemClick"))
        {
            out << "        try {\n";
            out << "            void* e_raw = nullptr;\n";
            out << "            if (winrt::impl::query_interface(args.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::IItemClickEventArgs>, &e_raw) >= 0 && e_raw) {\n";
            out << "                auto e = winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs::from_abi(e_raw);\n";
            out << "                auto clicked_item = e.ClickedItem();\n";
            out << "                if (clicked_item.get_abi()) {\n";
            out << "                    auto iid_map = winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::IInspectable>::iid();\n";
            out << "                    void* map_raw = nullptr;\n";
            out << "                    if (winrt::impl::query_interface(clicked_item.get_abi(), iid_map, &map_raw) >= 0 && map_raw) {\n";
            out << "                        auto uid_v = theme_find_in_raw_hstring_map(map_raw, L\"UniqueId\");\n";
            out << "                        auto uid = winrt::unbox_value_or<winrt::hstring>(uid_v, L\"\");\n";
            out << "                        auto __recent_title = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(map_raw, L\"Title\"), L\"\");\n";
            out << "                        auto __recent_subtitle = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(map_raw, L\"Subtitle\"), L\"\");\n";
            out << "                        auto __recent_image = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(map_raw, L\"ImagePath\"), L\"\");\n";
            out << "                        bool __recent_is_new = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(map_raw, L\"IsNew\"), false);\n";
            out << "                        bool __recent_is_updated = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(map_raw, L\"IsUpdated\"), false);\n";
            out << "                        bool __recent_is_experimental = winrt::unbox_value_or<bool>(theme_find_in_raw_hstring_map(map_raw, L\"IsExperimental\"), false);\n";
            out << "                        auto* vt = *reinterpret_cast<void* const**>(map_raw);\n";
            out << "                        using release_t = std::uint32_t(__stdcall*)(void*);\n";
            out << "                        reinterpret_cast<release_t>(vt[2])(map_raw);\n";
            out << "                        if (!uid.empty()) {\n";
            out << "                            load_recent_history();\n";
            out << "                            std::wstring __recent_id(uid.to_view());\n";
            out << "                            for (auto __it = get_recent_history_state().recently_visited_ids.begin(); __it != get_recent_history_state().recently_visited_ids.end();) {\n";
            out << "                                if (*__it == __recent_id) __it = get_recent_history_state().recently_visited_ids.erase(__it); else ++__it;\n";
            out << "                            }\n";
            out << "                            get_recent_history_state().recently_visited_ids.insert(get_recent_history_state().recently_visited_ids.begin(), __recent_id);\n";
            out << "                            for (auto __it = get_recent_history_state().recently_visited_items.begin(); __it != get_recent_history_state().recently_visited_items.end();) {\n";
            out << "                                if (__it->id == __recent_id) __it = get_recent_history_state().recently_visited_items.erase(__it); else ++__it;\n";
            out << "                            }\n";
            out << "                            RecentItemData __recent_record;\n";
            out << "                            __recent_record.id = __recent_id; __recent_record.title = __recent_title.c_str();\n";
            out << "                            __recent_record.subtitle = __recent_subtitle.c_str(); __recent_record.image_path = __recent_image.c_str();\n";
            out << "                            __recent_record.is_new = __recent_is_new; __recent_record.is_updated = __recent_is_updated;\n";
            out << "                            __recent_record.is_experimental = __recent_is_experimental;\n";
            out << "                            get_recent_history_state().recently_visited_items.insert(get_recent_history_state().recently_visited_items.begin(), std::move(__recent_record));\n";
            out << "                            save_recent_history();\n";
            for (auto const& source_name : recently_visited_source_vars) {
                std::string cap_name = source_name;
                if (!cap_name.empty()) cap_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cap_name[0])));
                std::string member_name = (cap_name == source_name && !element_named_types.count(source_name))
                    ? ("m_" + source_name) : source_name;
                out << "                            { void* __recent_raw = nullptr;\n";
                out << "                              if (winrt::IUnknown::qi_raw(" << member_name
                    << ".get_abi(), winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::iid(), &__recent_raw) >= 0 && __recent_raw) {\n";
                out << "                                  auto __recent_vector = winrt::Windows::Foundation::Collections::IVector<winrt::IInspectable>::from_abi(__recent_raw);\n";
                out << "                                  for (std::uint32_t __i = __recent_vector.get_Size(); __i > 0; --__i) {\n";
                out << "                                      auto __existing_item = __recent_vector.GetAt(__i - 1); void* __existing_map_raw = nullptr;\n";
                out << "                                      if (winrt::IUnknown::qi_raw(__existing_item.get_abi(), iid_map, &__existing_map_raw) >= 0 && __existing_map_raw) {\n";
                out << "                                          auto __existing_uid = winrt::unbox_value_or<winrt::hstring>(theme_find_in_raw_hstring_map(__existing_map_raw, L\"UniqueId\"), L\"\");\n";
                out << "                                          auto* __existing_vt = *reinterpret_cast<void* const**>(__existing_map_raw); reinterpret_cast<release_t>(__existing_vt[2])(__existing_map_raw);\n";
                out << "                                          if (__existing_uid == uid) __recent_vector.RemoveAt(__i - 1);\n";
                out << "                                      }\n";
                out << "                                  }\n";
                out << "                                  __recent_vector.InsertAt(0, clicked_item);\n";
                out << "                              }\n";
                out << "                            }\n";
            }
            for (auto const& element_name : recent_section_element_names)
                if (has_elem(element_name))
                    out << "                            if (" << element_name << ".m_unknown.get()) " << element_name << ".Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);\n";
            out << "                            if (g_theme_state.navigate_to) g_theme_state.navigate_to(uid.c_str());\n";
            out << "                        }\n";
            out << "                    }\n";
            out << "                }\n";
            out << "            }\n";
            out << "        } catch (...) { winrt::report_current_exception(\"item click\"); }\n";
        }
        if (cb_it != view.callbacks.end())
        {
            auto trim_callback_stmt = [](std::string value) {
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'
                    || value.front() == '\r' || value.front() == '\n')) value.erase(value.begin());
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t'
                    || value.back() == '\r' || value.back() == '\n' || value.back() == ';')) value.pop_back();
                return value;
            };
            auto matching_delimiter = [](std::string const& text, std::size_t open, char left, char right) {
                int depth = 0;
                bool in_string = false;
                bool escaped = false;
                for (std::size_t i = open; i < text.size(); ++i) {
                    char ch = text[i];
                    if (in_string) {
                        if (escaped) escaped = false;
                        else if (ch == '\\') escaped = true;
                        else if (ch == '"') in_string = false;
                        continue;
                    }
                    if (ch == '"') { in_string = true; continue; }
                    if (ch == left) ++depth;
                    else if (ch == right && --depth == 0) return i;
                }
                return std::string::npos;
            };
            auto split_callback_body = [&](std::string const& body) {
                std::vector<std::string> statements;
                std::size_t start = 0;
                int parens = 0, braces = 0;
                bool in_string = false, escaped = false;
                for (std::size_t i = 0; i < body.size(); ++i) {
                    char ch = body[i];
                    if (in_string) {
                        if (escaped) escaped = false;
                        else if (ch == '\\') escaped = true;
                        else if (ch == '"') in_string = false;
                        continue;
                    }
                    if (ch == '"') { in_string = true; continue; }
                    if (ch == '(') ++parens;
                    else if (ch == ')' && parens > 0) --parens;
                    else if (ch == '{') ++braces;
                    else if (ch == '}' && braces > 0) --braces;
                    else if (ch == ';' && parens == 0 && braces == 0) {
                        statements.push_back(body.substr(start, i - start));
                        start = i + 1;
                    }
                }
                if (start < body.size()) statements.push_back(body.substr(start));
                return statements;
            };
            std::vector<std::string> callback_statements;
            std::function<void(std::string const&, unsigned)> expand_callback_stmt;
            expand_callback_stmt = [&](std::string const& raw, unsigned depth) {
                std::string stmt = trim_callback_stmt(raw);
                if (stmt.empty()) return;
                if (depth < 32 && stmt.rfind("if", 0) == 0
                    && (stmt.size() == 2 || std::isspace(static_cast<unsigned char>(stmt[2])) || stmt[2] == '(')) {
                    auto open_paren = stmt.find('(');
                    auto close_paren = open_paren == std::string::npos
                        ? std::string::npos : matching_delimiter(stmt, open_paren, '(', ')');
                    auto open_brace = close_paren == std::string::npos
                        ? std::string::npos : stmt.find('{', close_paren + 1);
                    auto close_brace = open_brace == std::string::npos
                        ? std::string::npos : matching_delimiter(stmt, open_brace, '{', '}');
                    if (close_brace != std::string::npos) {
                        callback_statements.push_back("if (" + stmt.substr(open_paren + 1,
                            close_paren - open_paren - 1) + ") {");
                        for (auto const& child : split_callback_body(
                            stmt.substr(open_brace + 1, close_brace - open_brace - 1)))
                            expand_callback_stmt(child, depth + 1);
                        callback_statements.push_back("}");
                        std::string tail = trim_callback_stmt(stmt.substr(close_brace + 1));
                        if (tail.rfind("else", 0) == 0) {
                            auto else_brace = tail.find('{', 4);
                            auto else_end = else_brace == std::string::npos
                                ? std::string::npos : matching_delimiter(tail, else_brace, '{', '}');
                            if (else_end != std::string::npos) {
                                callback_statements.push_back("else {");
                                for (auto const& child : split_callback_body(
                                    tail.substr(else_brace + 1, else_end - else_brace - 1)))
                                    expand_callback_stmt(child, depth + 1);
                                callback_statements.push_back("}");
                                return;
                            }
                        }
                        return;
                    }
                }
                callback_statements.push_back(std::move(stmt));
            };
            for (auto const& raw_stmt : cb_it->second)
                expand_callback_stmt(raw_stmt, 0);

            for (auto const& raw_stmt : callback_statements)
            {
                auto trim = [](std::string s) {
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n' || s.back() == ';')) s.pop_back();
                    return s;
                };
                std::string stmt = trim(raw_stmt);
                if (stmt.empty()) continue;
                auto is_ident = [](std::string const& s) {
                    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
                    auto alnum = [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); };
                    if (s.empty() || !alpha(s[0])) return false;
                    for (char c : s) if (!alnum(c)) return false;
                    return true;
                };
                auto cpp_qualified = [](std::string const& ns, std::string const& type) {
                    std::string out = "winrt::";
                    for (char ch : ns) {
                        if (ch == '.') out += "::";
                        else out += ch;
                    }
                    return out + "::" + type;
                };
                auto rewrite_enum_values = [&](std::string expression) {
                    for (std::size_t p = 0; p < expression.size();) {
                        if (!(std::isalpha(static_cast<unsigned char>(expression[p])) || expression[p] == '_')) { ++p; continue; }
                        std::size_t name_end = p + 1;
                        while (name_end < expression.size()
                            && (std::isalnum(static_cast<unsigned char>(expression[name_end])) || expression[name_end] == '_')) ++name_end;
                        std::string enum_name = expression.substr(p, name_end - p);
                        bool already_qualified = p > 0
                            && (expression[p - 1] == '.' || expression[p - 1] == ':');
                        if (name_end < expression.size() && expression[name_end] == '.') {
                            std::size_t member_begin = name_end + 1, member_end = member_begin;
                            while (member_end < expression.size()
                                && (std::isalnum(static_cast<unsigned char>(expression[member_end])) || expression[member_end] == '_')) ++member_end;
                            auto eit = g_enum_info.find(enum_name);
                            if (eit != g_enum_info.end()
                                && eit->second.second.count(expression.substr(member_begin, member_end - member_begin))) {
                                std::string const& enum_namespace = eit->second.first;
                                std::size_t namespace_begin = p > enum_namespace.size()
                                    ? p - enum_namespace.size() - 1 : std::string::npos;
                                bool fully_qualified = namespace_begin != std::string::npos
                                    && expression.compare(namespace_begin, enum_namespace.size(), enum_namespace) == 0
                                    && expression[namespace_begin + enum_namespace.size()] == '.'
                                    && (namespace_begin == 0
                                        || !(std::isalnum(static_cast<unsigned char>(expression[namespace_begin - 1]))
                                            || expression[namespace_begin - 1] == '_'
                                            || expression[namespace_begin - 1] == '.'));
                                if (already_qualified && !fully_qualified) {
                                    p = name_end;
                                    continue;
                                }
                                std::size_t replace_begin = fully_qualified ? namespace_begin : p;
                                std::string qualified = cpp_qualified(enum_namespace, enum_name) + "::"
                                    + expression.substr(member_begin, member_end - member_begin);
                                expression.replace(replace_begin, member_end - replace_begin, qualified);
                                p = replace_begin + qualified.size();
                                continue;
                            }
                        }
                        p = name_end;
                    }
                    return expression;
                };
                auto rewrite_static_property_reads = [&](std::string expression) {
                    for (std::size_t p = 0; p < expression.size();) {
                        if (!(std::isalpha(static_cast<unsigned char>(expression[p])) || expression[p] == '_')) {
                            ++p;
                            continue;
                        }
                        std::size_t type_end = p + 1;
                        while (type_end < expression.size()
                            && (std::isalnum(static_cast<unsigned char>(expression[type_end])) || expression[type_end] == '_'))
                            ++type_end;
                        if (type_end >= expression.size() || expression[type_end] != '.') {
                            p = type_end;
                            continue;
                        }
                        std::size_t property_begin = type_end + 1, property_end = property_begin;
                        while (property_end < expression.size()
                            && (std::isalnum(static_cast<unsigned char>(expression[property_end])) || expression[property_end] == '_'))
                            ++property_end;
                        if (property_end == property_begin
                            || (property_end < expression.size() && expression[property_end] == '(')) {
                            p = property_end;
                            continue;
                        }
                        std::string type_name = expression.substr(p, type_end - p);
                        std::string property_name = expression.substr(property_begin, property_end - property_begin);
                        auto type_ns = g_type_ns.find(type_name);
                        std::string type_key = type_name;
                        for (auto& ch : type_key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        std::string property_key = property_name;
                        for (auto& ch : property_key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        auto type_properties = g_type_prop_kinds.find(type_key);
                        if (type_ns == g_type_ns.end() || g_enum_info.count(type_name)
                            || named_elements.count(type_name)
                            || type_properties == g_type_prop_kinds.end()
                            || !type_properties->second.count(property_key)) {
                            p = property_end;
                            continue;
                        }
                        std::string replacement = cpp_qualified(type_ns->second, type_name)
                            + "::get_" + property_name + "()";
                        expression.replace(p, property_end - p, replacement);
                        p += replacement.size();
                    }
                    return expression;
                };
                auto is_view_base_property_name = [&](std::string const& name) {
                    std::string base_name = view.base_type;
                    auto separator = base_name.find_last_of(".:");
                    if (separator != std::string::npos) base_name = base_name.substr(separator + 1);
                    for (auto& ch : base_name)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    std::string property_name = name;
                    for (auto& ch : property_name)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    auto properties = g_type_prop_kinds.find(base_name);
                    return properties != g_type_prop_kinds.end()
                        && properties->second.count(property_name) != 0;
                };
                auto is_state_variable_name = [&](std::string const& name) {
                    for (auto const& state : view.state_vars) {
                        if (state.name.size() != name.size()) continue;
                        bool same = true;
                        for (std::size_t i = 0; i < name.size(); ++i)
                            if (std::tolower(static_cast<unsigned char>(state.name[i]))
                                != std::tolower(static_cast<unsigned char>(name[i]))) {
                                same = false;
                                break;
                            }
                        if (same) return true;
                    }
                    return false;
                };
                auto rewrite_static_method_calls = [&](std::string expression) {
                    auto is_state_name = [&](std::string const& name) {
                        return is_state_variable_name(name);
                    };
                    bool in_string = false;
                    bool escaped = false;
                    for (std::size_t p = 0; p < expression.size();) {
                        char ch = expression[p];
                        if (in_string) {
                            if (escaped) escaped = false;
                            else if (ch == '\\') escaped = true;
                            else if (ch == '"') in_string = false;
                            ++p;
                            continue;
                        }
                        if (ch == '"') { in_string = true; ++p; continue; }
                        if (!(std::isalpha(static_cast<unsigned char>(ch)) || ch == '_')) {
                            ++p;
                            continue;
                        }
                        std::size_t type_end = p + 1;
                        while (type_end < expression.size()
                            && (std::isalnum(static_cast<unsigned char>(expression[type_end])) || expression[type_end] == '_'))
                            ++type_end;
                        bool root_identifier = p == 0
                            || !(std::isalnum(static_cast<unsigned char>(expression[p - 1]))
                                || expression[p - 1] == '_' || expression[p - 1] == '.');
                        if (!root_identifier || type_end + 1 >= expression.size()
                            || expression[type_end] != '.') {
                            p = type_end;
                            continue;
                        }
                        std::size_t method_begin = type_end + 1, method_end = method_begin;
                        while (method_end < expression.size()
                            && (std::isalnum(static_cast<unsigned char>(expression[method_end])) || expression[method_end] == '_'))
                            ++method_end;
                        if (method_end == method_begin || method_end >= expression.size()
                            || expression[method_end] != '(') {
                            p = method_end;
                            continue;
                        }
                        std::string type_name = expression.substr(p, type_end - p);
                        auto type_ns = g_type_ns.find(type_name);
                        if (type_ns == g_type_ns.end() || named_elements.count(type_name)
                            || is_state_name(type_name)) {
                            p = method_end;
                            continue;
                        }
                        if (is_view_base_property_name(type_name)) {
                            std::string replacement = "this->" + type_name + "()";
                            expression.replace(p, type_end - p, replacement);
                            p += replacement.size();
                            continue;
                        }
                        std::string method_key = expression.substr(method_begin, method_end - method_begin);
                        for (auto& c : method_key)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        std::string type_key = type_name;
                        for (auto& c : type_key)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        auto static_methods = g_type_static_methods.find(type_key);
                        if (static_methods == g_type_static_methods.end()
                            || !static_methods->second.count(method_key)) {
                            p = method_end;
                            continue;
                        }
                        std::string replacement = cpp_qualified(type_ns->second, type_name)
                            + "::" + expression.substr(method_begin, method_end - method_begin);
                        expression.replace(p, method_end - p, replacement);
                        p += replacement.size();
                    }
                    return expression;
                };
                auto rewrite_element_reads = [&](std::string expression) {
                    auto metadata_type_key = [](std::string type_name) {
                        auto generic = type_name.find('<');
                        if (generic != std::string::npos) type_name.resize(generic);
                        auto separator = type_name.find_last_of(".:");
                        if (separator != std::string::npos) type_name = type_name.substr(separator + 1);
                        for (auto& ch : type_name)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        return type_name;
                    };
                    auto collection_type = [&](std::string const& type_name) {
                        std::string key = metadata_type_key(type_name);
                        return key == "itemcollection" || key.find("vector") != std::string::npos
                            || key.find("collection") != std::string::npos || key.find("list") != std::string::npos;
                    };
                    auto rewrite_element = [&](std::string const& name, std::string current_type) {
                        std::size_t from = 0;
                        while ((from = expression.find(name, from)) != std::string::npos) {
                            std::size_t end = from + name.size();
                            bool boundary = from == 0
                                || !(std::isalnum(static_cast<unsigned char>(expression[from - 1])) || expression[from - 1] == '_');
                            bool this_access = from >= 6 && expression.compare(from - 6, 6, "this->") == 0;
                            bool member_access = (from > 0 && expression[from - 1] == '.')
                                || (from >= 2 && expression.compare(from - 2, 2, "->") == 0);
                            if (this_access) member_access = false;
                            if (!boundary || member_access
                                || end >= expression.size() || expression[end] != '.') { from = end; continue; }
                            std::size_t dot = end;
                            while (dot < expression.size() && expression[dot] == '.') {
                                std::size_t member = dot + 1, member_end = member;
                                while (member_end < expression.size()
                                    && (std::isalnum(static_cast<unsigned char>(expression[member_end])) || expression[member_end] == '_')) ++member_end;
                                if (member_end == member) break;
                                std::string member_name = expression.substr(member, member_end - member);
                                if (member_name == "Count" && collection_type(current_type)) {
                                    expression.replace(member, member_end - member, "Size");
                                    member_end = member + 4;
                                    member_name = "Size";
                                }
                                if (member_name != "Count") {
                                    auto properties = g_type_prop_kinds.find(metadata_type_key(current_type));
                                    if (properties != g_type_prop_kinds.end()) {
                                        std::string key = member_name;
                                        for (auto& ch : key)
                                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                        if (auto property = properties->second.find(key); property != properties->second.end())
                                            current_type = property->second;
                                        else
                                            current_type.clear();
                                    } else {
                                        current_type.clear();
                                    }
                                }
                                if (member_end >= expression.size() || expression[member_end] != '(') {
                                    expression.insert(member_end, "()");
                                    dot = member_end + 2;
                                } else {
                                    auto call_end = expression.find(')', member_end + 1);
                                    if (call_end == std::string::npos) break;
                                    dot = call_end + 1;
                                }
                            }
                            from = end;
                        }
                    };
                    for (auto const& [name, type] : named_elements) {
                        rewrite_element(name, type);
                    }
                    for (auto const& state : view.state_vars) {
                        rewrite_element(state.name, state.type);
                    }
                    if (uses_main_window_access) {
                        for (auto const& [type, name] : view.main_window_controls) {
                            (void)type;
                            rewrite_element(name, type);
                        }
                    }
                    return expression;
                };
                auto is_view_base_property = [&](std::string const& name) {
                    if (named_elements.count(name)) return false;
                    for (auto const& state : view.state_vars) {
                        if (state.name.size() != name.size()) continue;
                        bool same = true;
                        for (std::size_t i = 0; i < name.size(); ++i)
                            if (std::tolower(static_cast<unsigned char>(state.name[i]))
                                != std::tolower(static_cast<unsigned char>(name[i]))) {
                                same = false;
                                break;
                            }
                        if (same) return false;
                    }
                    std::string base_name = view.base_type;
                    auto separator = base_name.find_last_of(".:");
                    if (separator != std::string::npos) base_name = base_name.substr(separator + 1);
                    for (auto& ch : base_name)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    std::string property_name = name;
                    for (auto& ch : property_name)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    auto properties = g_type_prop_kinds.find(base_name);
                    return properties != g_type_prop_kinds.end()
                        && properties->second.count(property_name) != 0;
                };
                auto rewrite_view_base_property_reads = [&](std::string expression) {
                    bool in_string = false;
                    bool escaped = false;
                    for (std::size_t p = 0; p < expression.size();) {
                        char ch = expression[p];
                        if (in_string) {
                            if (escaped) escaped = false;
                            else if (ch == '\\') escaped = true;
                            else if (ch == '"') in_string = false;
                            ++p;
                            continue;
                        }
                        if (ch == '"') { in_string = true; ++p; continue; }
                        if (!(std::isalpha(static_cast<unsigned char>(ch)) || ch == '_')) {
                            ++p;
                            continue;
                        }
                        std::size_t end = p + 1;
                        while (end < expression.size()
                            && (std::isalnum(static_cast<unsigned char>(expression[end])) || expression[end] == '_')) ++end;
                        bool root_identifier = p == 0
                            || !(std::isalnum(static_cast<unsigned char>(expression[p - 1]))
                                || expression[p - 1] == '_' || expression[p - 1] == '.');
                        bool member_chain = end < expression.size()
                            && (expression[end] == '.' || expression[end] == '(');
                        std::string name = expression.substr(p, end - p);
                        if (!root_identifier || !member_chain || g_enum_info.count(name)
                            || (p >= 2 && expression.compare(p - 2, 2, "->") == 0)
                            || !is_view_base_property(name)) {
                            p = end;
                            continue;
                        }
                        std::string replacement = "this->" + name + "()";
                        expression.replace(p, end - p, replacement);
                        p += replacement.size();
                    }
                    return expression;
                };
                auto rewrite_main_window_access = [&](std::string expression) {
                    if (!uses_main_window_access) return expression;
                    std::size_t from = 0;
                    while ((from = expression.find(main_window_access_prefix, from)) != std::string::npos) {
                        bool boundary = from == 0
                            || !(std::isalnum(static_cast<unsigned char>(expression[from - 1])) || expression[from - 1] == '_');
                        if (!boundary) {
                            from += main_window_access_prefix.size();
                            continue;
                        }
                        std::size_t member_begin = from + main_window_access_prefix.size();
                        std::size_t member_end = member_begin;
                        while (member_end < expression.size()
                            && (std::isalnum(static_cast<unsigned char>(expression[member_end])) || expression[member_end] == '_'))
                            ++member_end;
                        if (member_end == member_begin) {
                            from = member_end;
                            continue;
                        }
                        std::string member = expression.substr(member_begin, member_end - member_begin);
                        std::string resolved_control;
                        std::string resolved_name;
                        for (auto const& [control, name] : view.main_window_controls) {
                            if (control != member && name != member) continue;
                            if (!resolved_control.empty()) {
                                resolved_control.clear();
                                resolved_name.clear();
                                break;
                            }
                            resolved_control = control;
                            resolved_name = name;
                        }
                        auto type_ns = g_type_ns.find(resolved_control);
                        if (resolved_control.empty() || resolved_name.empty() || type_ns == g_type_ns.end()) {
                            from = member_end;
                            continue;
                        }
                        std::string replacement = "[]() { auto __main_window_control = ::main_window_control<"
                            + cpp_qualified(type_ns->second, resolved_control)
                            + ">(L\"" + resolved_control + "\"); return __main_window_control";
                        expression.replace(from, member_end - from, replacement);
                        std::size_t tail = from + replacement.size();
                        while (tail < expression.size() && expression[tail] == '.') {
                            std::size_t tail_member = tail + 1;
                            std::size_t tail_end = tail_member;
                            while (tail_end < expression.size()
                                && (std::isalnum(static_cast<unsigned char>(expression[tail_end])) || expression[tail_end] == '_'))
                                ++tail_end;
                            if (tail_end == tail_member) break;
                            if (tail_end >= expression.size() || expression[tail_end] != '(') {
                                expression.insert(tail_end, "()");
                                tail = tail_end + 2;
                            } else {
                                tail = tail_end + 1;
                            }
                        }
                        expression.insert(tail, "; }()");
                        from = tail;
                    }
                    return expression;
                };
                auto rewrite_callback_expr = [&](std::string expression) {
                    for (std::size_t p = 0; (p = expression.find("self->", p)) != std::string::npos;)
                        expression.replace(p, 6, "this->");
                    for (std::size_t p = 0; (p = expression.find("null", p)) != std::string::npos;) {
                        std::size_t null_length = expression.compare(p, 7, "nullptr") == 0 ? 7 : 4;
                        bool left_boundary = p == 0
                            || !(std::isalnum(static_cast<unsigned char>(expression[p - 1])) || expression[p - 1] == '_');
                        bool right_boundary = p + null_length >= expression.size()
                            || !(std::isalnum(static_cast<unsigned char>(expression[p + null_length])) || expression[p + null_length] == '_');
                        if (!left_boundary || !right_boundary) {
                            p += null_length;
                            continue;
                        }
                        std::size_t lhs_end = p;
                        while (lhs_end > 0 && std::isspace(static_cast<unsigned char>(expression[lhs_end - 1]))) --lhs_end;
                        std::size_t op_begin = lhs_end;
                        if (op_begin >= 2 && (expression.compare(op_begin - 2, 2, "!=") == 0
                            || expression.compare(op_begin - 2, 2, "==") == 0))
                            op_begin -= 2;
                        if (op_begin != lhs_end) {
                            std::size_t cursor = op_begin;
                            while (cursor > 0 && std::isspace(static_cast<unsigned char>(expression[cursor - 1]))) --cursor;
                            std::size_t lhs_begin = cursor;
                            while (cursor > 0) {
                                if (expression[cursor - 1] == ')') {
                                    std::size_t open = cursor - 1;
                                    std::size_t depth = 1;
                                    while (open > 0 && depth != 0) {
                                        --open;
                                        if (expression[open] == ')') ++depth;
                                        else if (expression[open] == '(') --depth;
                                    }
                                    if (depth != 0) break;
                                    cursor = open;
                                }
                                std::size_t ident_end = cursor;
                                while (cursor > 0) {
                                    char ch = expression[cursor - 1];
                                    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')
                                        --cursor;
                                    else
                                        break;
                                }
                                if (cursor == ident_end) break;
                                lhs_begin = cursor;
                                if (cursor > 0 && expression[cursor - 1] == '.') {
                                    --cursor;
                                    continue;
                                }
                                if (cursor >= 2 && expression.compare(cursor - 2, 2, "->") == 0) {
                                    cursor -= 2;
                                    continue;
                                }
                                break;
                            }
                            if (lhs_begin < op_begin) {
                                std::string lhs = expression.substr(lhs_begin, op_begin - lhs_begin);
                                bool is_not_null = expression.compare(op_begin, 2, "!=") == 0;
                                std::string replacement = is_not_null
                                    ? "(" + lhs + ".get_abi() != nullptr)"
                                    : "(" + lhs + ".get_abi() == nullptr)";
                                expression.replace(lhs_begin, p + null_length - lhs_begin, replacement);
                                p = lhs_begin + replacement.size();
                                continue;
                            }
                        }
                        if (null_length == 4) expression.replace(p, null_length, "nullptr");
                        p += 7;
                    }
                    for (auto const& state : view.state_vars) {
                        std::string state_type = state.type;
                        for (auto& ch : state_type)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        bool has_scalar_accessor = state_type == "string" || state_type == "int"
                            || state_type == "double" || state_type == "bool" || state_type == "color"
                            || state_type == "any" || state_type == "object" || state_type == "hstring"
                            || state_type == "winrt::hstring" || state_type == "boolean"
                            || state_type == "integer" || state_type == "any[]" || state_type == "object[]"
                            || (state_type.size() > 2 && state_type.substr(state_type.size() - 2) == "[]");
                        if (!has_scalar_accessor || inferred_types.count(state.name)
                            || element_named_types.count(state.name))
                            continue;
                        std::string property_name = state.name;
                        if (!property_name.empty())
                            property_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(property_name[0])));
                        if (named_elements.count(property_name))
                            continue;
                        std::size_t p = 0;
                        while ((p = expression.find(property_name, p)) != std::string::npos) {
                            std::size_t end = p + property_name.size();
                            bool boundary = p == 0
                                || !(std::isalnum(static_cast<unsigned char>(expression[p - 1])) || expression[p - 1] == '_');
                            bool member_access = p >= 2 && expression.compare(p - 2, 2, "->") == 0;
                            if (!boundary || member_access || (end < expression.size()
                                && (std::isalnum(static_cast<unsigned char>(expression[end])) || expression[end] == '_' || expression[end] == '('))) {
                                p = end;
                                continue;
                            }
                            expression.insert(end, "()");
                            p = end + 2;
                        }
                    }
                    auto metadata_type_key = [](std::string type_name) {
                        auto generic = type_name.find('<');
                        if (generic != std::string::npos) type_name.resize(generic);
                        auto separator = type_name.find_last_of(".:");
                        if (separator != std::string::npos) type_name = type_name.substr(separator + 1);
                        for (auto& ch : type_name)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        return type_name;
                    };
                    auto is_color_type = [&](std::string type_name) {
                        return metadata_type_key(std::move(type_name)) == "color";
                    };
                    auto is_color_property = [&](std::string const& property_name) {
                        std::string key = property_name;
                        for (auto& ch : key)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        for (auto const& state : view.state_vars) {
                            std::string state_name = state.name;
                            for (auto& ch : state_name)
                                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            if (state_name == key && is_color_type(state.type))
                                return true;
                        }
                        if (auto own_type = g_type_prop_kinds.find(metadata_type_key(view.name));
                            own_type != g_type_prop_kinds.end()) {
                            if (auto property = own_type->second.find(key); property != own_type->second.end())
                                return is_color_type(property->second);
                        }
                        bool found = false;
                        for (auto const& [type_name, properties] : g_type_prop_kinds) {
                            (void)type_name;
                            auto property = properties.find(key);
                            if (property == properties.end()) continue;
                            found = true;
                            if (!is_color_type(property->second)) return false;
                        }
                        return found;
                    };
                    std::size_t to_string = 0;
                    while ((to_string = expression.find(".ToString()", to_string)) != std::string::npos) {
                        std::size_t receiver_end = to_string;
                        std::size_t cursor = receiver_end;
                        if (cursor > 0 && expression[cursor - 1] == ')') {
                            int nesting = 1;
                            --cursor;
                            while (cursor > 0 && nesting > 0) {
                                --cursor;
                                if (expression[cursor] == ')') ++nesting;
                                else if (expression[cursor] == '(') --nesting;
                            }
                        }
                        while (cursor > 0
                            && (std::isalnum(static_cast<unsigned char>(expression[cursor - 1]))
                                || expression[cursor - 1] == '_')) --cursor;
                        while (cursor > 0 && expression[cursor - 1] == '.') {
                            --cursor;
                            while (cursor > 0
                                && (std::isalnum(static_cast<unsigned char>(expression[cursor - 1]))
                                    || expression[cursor - 1] == '_')) --cursor;
                        }
                        std::size_t property_end = receiver_end;
                        if (property_end >= 2 && expression.compare(property_end - 2, 2, "()") == 0)
                            property_end -= 2;
                        std::size_t property_begin = property_end;
                        while (property_begin > 0
                            && (std::isalnum(static_cast<unsigned char>(expression[property_begin - 1]))
                                || expression[property_begin - 1] == '_')) --property_begin;
                        std::string property_name = expression.substr(property_begin, property_end - property_begin);
                        if (!is_color_property(property_name)) {
                            to_string += 11;
                            continue;
                        }
                        std::string receiver = expression.substr(cursor, receiver_end - cursor);
                        if (receiver.empty() || receiver.back() != ')') receiver += "()";
                        std::string replacement = "format_color(" + receiver + ")";
                        expression.replace(cursor, to_string + 11 - cursor, replacement);
                        to_string = cursor + replacement.size();
                    }
                    expression = rewrite_main_window_access(std::move(expression));
                    expression = rewrite_element_reads(std::move(expression));
                    expression = rewrite_view_base_property_reads(std::move(expression));
                    for (std::size_t p = 0; (p = expression.find("FinalView().HorizontalOffset()", p)) != std::string::npos;) {
                        std::size_t start = (p >= 5 && expression.compare(p - 5, 5, "args.") == 0) ? p - 5 : p;
                        std::string replacement = "([&]() -> double { if (args.get_abi()) { auto __fv = args.FinalView(); if (__fv.m_unknown.get()) return __fv.HorizontalOffset(); } if (scroller.m_unknown.get()) return scroller.HorizontalOffset(); return 0.0; })()";
                        expression.replace(start, (p + 30) - start, replacement);
                        p = start + replacement.size();
                    }
                    for (std::size_t p = 0; (p = expression.find("FinalView().VerticalOffset()", p)) != std::string::npos;) {
                        std::size_t start = (p >= 5 && expression.compare(p - 5, 5, "args.") == 0) ? p - 5 : p;
                        std::string replacement = "([&]() -> double { if (args.get_abi()) { auto __fv = args.FinalView(); if (__fv.m_unknown.get()) return __fv.VerticalOffset(); } if (scroller.m_unknown.get()) return scroller.VerticalOffset(); return 0.0; })()";
                        expression.replace(start, (p + 28) - start, replacement);
                        p = start + replacement.size();
                    }
                    std::vector<std::pair<std::string, std::string>> element_types;
                    element_types.reserve(named_elements.size() + view.state_vars.size());
                    for (auto const& [element, projected_type] : named_elements)
                        element_types.emplace_back(element, projected_type);
                    for (auto const& state : view.state_vars) {
                        if (std::none_of(element_types.begin(), element_types.end(),
                            [&](auto const& entry) { return entry.first == state.name; }))
                            element_types.emplace_back(state.name, state.type);
                    }
                    for (auto const& [element, projected_type] : element_types) {
                        std::string type_key = projected_type;
                        if (auto const scope = type_key.rfind("::"); scope != std::string::npos)
                            type_key = type_key.substr(scope + 2);
                        for (auto& ch : type_key)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        auto properties = g_type_prop_kinds.find(type_key);
                        if (properties == g_type_prop_kinds.end()) continue;
                        std::size_t from = 0;
                        while ((from = expression.find(element, from)) != std::string::npos) {
                            std::size_t end = from + element.size();
                            bool boundary = from == 0
                                || !(std::isalnum(static_cast<unsigned char>(expression[from - 1])) || expression[from - 1] == '_');
                            if (!boundary || end + 1 >= expression.size()
                                || expression[end] != '.') {
                                from = end;
                                continue;
                            }
                            std::size_t property_end = end + 1;
                            while (property_end < expression.size()
                                && (std::isalnum(static_cast<unsigned char>(expression[property_end])) || expression[property_end] == '_'))
                                ++property_end;
                            if (property_end == end + 1
                                || property_end + 1 >= expression.size()
                                || expression.compare(property_end, 2, "()") != 0) {
                                from = property_end;
                                continue;
                            }
                            std::string property_key = expression.substr(end + 1, property_end - end - 1);
                            for (auto& ch : property_key)
                                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            auto property_type = properties->second.find(property_key);
                            if (property_type == properties->second.end()) {
                                from = property_end + 2;
                                continue;
                            }
                            std::string value_type = property_type->second;
                            for (auto& ch : value_type)
                                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            bool nullable_bool = false;
                            auto reference_start = value_type.find("ireference<");
                            if (reference_start != std::string::npos) {
                                auto argument_start = reference_start + std::string_view("ireference<").size();
                                auto argument_end = value_type.find('>', argument_start);
                                if (argument_end != std::string::npos) {
                                    auto argument = value_type.substr(argument_start, argument_end - argument_start);
                                    nullable_bool = argument == "bool" || argument == "boolean";
                                }
                            }
                            std::size_t read_end = property_end + 2;
                            std::string read = expression.substr(from, read_end - from);
                            if (nullable_bool && (read_end >= expression.size()
                                || (expression[read_end] != '.' && expression[read_end] != '('))) {
                                std::string replacement = "winrt::unbox_value_or<bool>(" + read + ", false)";
                                expression.replace(from, read.size(), replacement);
                                from += replacement.size();
                                continue;
                            }
                            std::string_view normalized_type = value_type;
                            bool boxed_value = normalized_type == "object" || normalized_type == "any"
                                || normalized_type == "inspectable" || normalized_type == "winrt::iinspectable";
                            std::size_t comparison = read_end;
                            while (comparison < expression.size()
                                && std::isspace(static_cast<unsigned char>(expression[comparison]))) ++comparison;
                            bool equality = expression.compare(comparison, 2, "==") == 0
                                || expression.compare(comparison, 2, "!=") == 0;
                            if (boxed_value && equality) {
                                comparison += 2;
                                while (comparison < expression.size()
                                    && std::isspace(static_cast<unsigned char>(expression[comparison]))) ++comparison;
                                if (comparison < expression.size() && expression[comparison] == 'L') ++comparison;
                                if (comparison < expression.size() && expression[comparison] == '"') {
                                    std::string replacement = "winrt::unbox_value_or<winrt::hstring>("
                                        + read + ", winrt::hstring{})";
                                    expression.replace(from, read.size(), replacement);
                                    from += replacement.size();
                                    continue;
                                }
                            }
                            from = read_end;
                        }
                    }
                    auto unique_property_type = [&](std::string const& property_key) {
                        std::string result;
                        for (auto const& [type_name, properties] : g_type_prop_kinds) {
                            (void)type_name;
                            auto property = properties.find(property_key);
                            if (property == properties.end()) continue;
                            auto candidate = metadata_type_key(property->second);
                            if (result.empty()) result = std::move(candidate);
                            else if (result != candidate) return std::string{};
                        }
                        return result;
                    };
                    for (std::string_view parameter_name : {std::string_view("sender"), std::string_view("args")}) {
                        std::size_t from = 0;
                        while ((from = expression.find(parameter_name, from)) != std::string::npos) {
                            std::size_t end = from + parameter_name.size();
                            bool boundary = from == 0
                                || !(std::isalnum(static_cast<unsigned char>(expression[from - 1]))
                                    || expression[from - 1] == '_');
                            if (!boundary || end >= expression.size() || expression[end] != '.') {
                                from = end;
                                continue;
                            }
                            std::size_t member = end + 1;
                            std::string current_type;
                            bool first_property = true;
                            while (member < expression.size()) {
                                std::size_t member_end = member;
                                while (member_end < expression.size()
                                    && (std::isalnum(static_cast<unsigned char>(expression[member_end]))
                                        || expression[member_end] == '_')) ++member_end;
                                if (member_end == member) break;
                                std::string property_key = expression.substr(member, member_end - member);
                                for (auto& ch : property_key)
                                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                std::string next_type;
                                if (first_property) {
                                    next_type = unique_property_type(property_key);
                                } else if (auto properties = g_type_prop_kinds.find(current_type);
                                    properties != g_type_prop_kinds.end()) {
                                    if (auto property = properties->second.find(property_key);
                                        property != properties->second.end())
                                        next_type = metadata_type_key(property->second);
                                }
                                if (next_type.empty()) {
                                    if (first_property && (member_end >= expression.size()
                                        || expression[member_end] != '(')) {
                                        expression.insert(member_end, "()");
                                        member_end += 2;
                                    }
                                    break;
                                }
                                if (member_end >= expression.size() || expression[member_end] != '(') {
                                    expression.insert(member_end, "()");
                                    member_end += 2;
                                }
                                current_type = std::move(next_type);
                                first_property = false;
                                if (member_end >= expression.size() || expression[member_end] != '.')
                                    break;
                                member = member_end + 1;
                            }
                            from = end + 1;
                        }
                    }
                    expression = rewrite_enum_values(std::move(expression));
                    expression = rewrite_element_reads(std::move(expression));
                    expression = rewrite_view_base_property_reads(std::move(expression));
                    for (std::size_t p = 0; (p = expression.find("FinalView().HorizontalOffset()", p)) != std::string::npos;) {
                        std::size_t start = (p >= 5 && expression.compare(p - 5, 5, "args.") == 0) ? p - 5 : p;
                        std::string replacement = "([&]() -> double { if (args.get_abi()) { auto __fv = args.FinalView(); if (__fv.m_unknown.get()) return __fv.HorizontalOffset(); } if (scroller.m_unknown.get()) return scroller.HorizontalOffset(); return 0.0; })()";
                        expression.replace(start, (p + 30) - start, replacement);
                        p = start + replacement.size();
                    }
                    for (std::size_t p = 0; (p = expression.find("FinalView().VerticalOffset()", p)) != std::string::npos;) {
                        std::size_t start = (p >= 5 && expression.compare(p - 5, 5, "args.") == 0) ? p - 5 : p;
                        std::string replacement = "([&]() -> double { if (args.get_abi()) { auto __fv = args.FinalView(); if (__fv.m_unknown.get()) return __fv.VerticalOffset(); } if (scroller.m_unknown.get()) return scroller.VerticalOffset(); return 0.0; })()";
                        expression.replace(start, (p + 28) - start, replacement);
                        p = start + replacement.size();
                    }
                    expression = rewrite_static_property_reads(std::move(expression));
                    expression = rewrite_static_method_calls(std::move(expression));
                    for (std::size_t p = 0; (p = expression.find("this->", p)) != std::string::npos;) {
                        std::size_t name_end = p + 6;
                        while (name_end < expression.size()
                            && (std::isalnum(static_cast<unsigned char>(expression[name_end]))
                                || expression[name_end] == '_')) ++name_end;
                        if (name_end > p + 6 && expression.compare(name_end, 2, "::") == 0) {
                            expression.erase(p, 6);
                            continue;
                        }
                        p = name_end;
                    }
                    return expression;
                };
                if (stmt.rfind("NavigateTo(", 0) == 0 && stmt.back() == ')') {
                    std::string route = trim(stmt.substr(11, stmt.size() - 12));
                    bool narrow_literal = route.size() >= 2 && route.front() == '"' && route.back() == '"';
                    bool wide_literal = route.size() >= 3 && route[0] == 'L' && route[1] == '"' && route.back() == '"';
                    if (narrow_literal || wide_literal) {
                        if (narrow_literal) route = "L" + route;
                        out << "        if (g_theme_state.navigate_to) g_theme_state.navigate_to(" << route << ");\n";
                        continue;
                    }
                }
                if (stmt.rfind("if (", 0) == 0 && stmt.size() >= 6 && stmt.back() == '{') {
                    auto close = stmt.rfind(')', stmt.size() - 1);
                    if (close != std::string::npos) {
                        std::string condition = trim(stmt.substr(4, close - 4));
                        out << "        if (" << rewrite_callback_expr(std::move(condition)) << ") {\n";
                        continue;
                    }
                }
                if (stmt == "else {") {
                    out << "        else {\n";
                    continue;
                }
                if (stmt == "}") {
                    out << "        }\n";
                    continue;
                }
                // <Element>.<Property> = <value>  -> named-element property setter
                bool is_setter = false;
                std::size_t eq = stmt.find('=');
                if (eq != std::string::npos && eq > 0)
                {
                    bool op_around = (eq + 1 < stmt.size() && (stmt[eq + 1] == '=' || stmt[eq + 1] == '>'));
                    op_around = op_around || (stmt[eq - 1] == '!' || stmt[eq - 1] == '<' || stmt[eq - 1] == '>' || stmt[eq - 1] == '=');
                    is_setter = !op_around;
                }
                if (is_setter)
                {
                    std::string lhs = trim(stmt.substr(0, eq));
                    std::string rhs = trim(stmt.substr(eq + 1));
                    auto dot = lhs.find('.');
                    if (uses_main_window_access) {
                        auto last_dot = lhs.rfind('.');
                        if (last_dot != std::string::npos && last_dot > 0 && last_dot + 1 < lhs.size()) {
                            std::string parent = trim(lhs.substr(0, last_dot));
                            if (parent.find(main_window_access_prefix) != std::string::npos) {
                                std::string resolved_parent = rewrite_main_window_access(std::move(parent));
                                if (resolved_parent.find("::main_window_control<") != std::string::npos) {
                                    out << "        " << resolved_parent << "." << lhs.substr(last_dot + 1)
                                        << "(" << rewrite_callback_expr(std::move(rhs)) << ");\n";
                                    continue;
                                }
                            }
                        }
                    }
                    if (dot != std::string::npos && dot > 0 && dot + 1 < lhs.size()
                        && lhs.find('.', dot + 1) == std::string::npos) {
                        std::string type_name = lhs.substr(0, dot);
                        std::string property_name = lhs.substr(dot + 1);
                        auto type_ns = g_type_ns.find(type_name);
                        if (is_ident(type_name) && is_ident(property_name) && type_ns != g_type_ns.end()
                            && !named_elements.count(type_name)) {
                            out << "        " << cpp_qualified(type_ns->second, type_name) << "::put_" << property_name
                                << "(" << rewrite_callback_expr(std::move(rhs)) << ");\n";
                            continue;
                        }
                    }
                    bool valid = dot != std::string::npos && dot > 0 && dot + 1 < lhs.size();
                    if (valid)
                    {
                        std::string elem = lhs.substr(0, dot);
                        std::string prop = lhs.substr(dot + 1);
                        auto named_element = named_elements.find(elem);
                        auto state_element = element_named_types.find(elem);
                        bool is_element = named_element != named_elements.end()
                            || state_element != element_named_types.end();
                        if (is_ident(elem) && is_ident(prop) && is_element && !rhs.empty())
                        {
                            std::string element_type = named_element != named_elements.end()
                                ? named_element->second : state_element->second;
                            // Projection getters are methods: a named-element
                            // chain used as a value needs "()" on each link.
                            std::string val_cpp = rewrite_callback_expr(std::move(rhs));
                            if (val_cpp == "nullptr") {
                                std::string metadata_type = element_type;
                                if (auto const scope = metadata_type.rfind("::"); scope != std::string::npos)
                                    metadata_type = metadata_type.substr(scope + 2);
                                for (auto& ch : metadata_type)
                                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                std::string property_key = prop;
                                for (auto& ch : property_key)
                                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                if (auto const properties = g_type_prop_kinds.find(metadata_type);
                                    properties != g_type_prop_kinds.end()) {
                                    if (auto const property_type = properties->second.find(property_key);
                                        property_type != properties->second.end()) {
                                        std::string value_type = property_type->second;
                                        if (value_type == "hstring")
                                            val_cpp = "winrt::hstring{}";
                                        else if (value_type == "IInspectable")
                                            val_cpp = "winrt::IInspectable{}";
                                        else {
                                            auto generic = value_type.find('<');
                                            std::string type_name = value_type.substr(0, generic);
                                            if (generic != std::string::npos && type_name == "IReference")
                                                val_cpp = "winrt::Windows::Foundation::" + value_type + "{}";
                                            else if (auto const type_ns = g_type_ns.find(type_name);
                                                type_ns != g_type_ns.end())
                                                val_cpp = cpp_qualified(type_ns->second, type_name) + "{}";
                                        }
                                    }
                                }
                            }
                            auto is_code_position = [&](std::size_t position) {
                                bool in_string = false;
                                bool escaped = false;
                                for (std::size_t i = 0; i < position; ++i)
                                {
                                    char ch = val_cpp[i];
                                    if (escaped) { escaped = false; continue; }
                                    if (in_string && ch == '\\') { escaped = true; continue; }
                                    if (ch == '"') in_string = !in_string;
                                }
                                return !in_string;
                            };
                            for (auto const& [nm, t] : named_elements)
                            {
                                std::size_t search_from = 0;
                                while (search_from < val_cpp.size())
                                {
                                    std::size_t p = val_cpp.find(nm, search_from);
                                    if (p == std::string::npos) break;
                                    std::size_t q = p + nm.size();
                                    bool identifier_boundary = p == 0
                                        || !(std::isalnum(static_cast<unsigned char>(val_cpp[p - 1])) || val_cpp[p - 1] == '_');
                                    if (identifier_boundary && is_code_position(p) && q < val_cpp.size() && val_cpp[q] == '.')
                                    {
                                        std::size_t i = q;
                                        while (i < val_cpp.size() && val_cpp[i] == '.')
                                        {
                                            std::size_t j = i + 1;
                                            while (j < val_cpp.size() && (std::isalnum(static_cast<unsigned char>(val_cpp[j])) || val_cpp[j] == '_')) ++j;
                                            if (j == i + 1) break;
                                            if (j >= val_cpp.size() || val_cpp[j] != '(')
                                            {
                                                val_cpp.insert(j, "()");
                                                i = j + 2;
                                            }
                                            else
                                                i = j + 1;
                                        }
                                    }
                                    search_from = q;
                                }
                            }
                            if (val_cpp.size() >= 2 && val_cpp.front() == '\"' && val_cpp.back() == '\"')
                                val_cpp = "L" + val_cpp;   // narrow literal -> wide for hstring props
                            // IsChecked takes IReference<bool> on checkable
                            // controls: a raw bool must be boxed.
                            std::string prop_lo2 = prop;
                            for (auto& ch : prop_lo2) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            if (prop_lo2 == "ischecked" && (val_cpp == "true" || val_cpp == "false"))
                            {
                                if (!is_plain_bool_ischecked(element_type))
                                    val_cpp = "winrt::Windows::Foundation::IReference<bool>(winrt::box_value(" + val_cpp + "))";
                            }
                            // A bare C# member (e.g. BarItems) has no projection:
                            // declare an IInspectable stub member so the wiring
                            // stays alive and compiles (the host layer populates
                            // it at runtime). Literals and known names are skipped.
                            {
                                bool bare_rhs = is_ident(val_cpp);
                                if (bare_rhs && val_cpp != "true" && val_cpp != "false" && val_cpp != "nullptr"
                                    && !named_elements.count(val_cpp))
                                {
                                    bool known_state = false;
                                    for (auto const& sv : view.state_vars)
                                    {
                                        std::string sv_lo = sv.name, vc_lo = val_cpp;
                                        for (auto& c : sv_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                        for (auto& c : vc_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                        if (sv_lo == vc_lo) { val_cpp = sv.name; known_state = true; break; }
                                    }
                                    if (!known_state) extra_members.insert(val_cpp);
                                }
                            }
                            out << "        " << elem << "." << prop << "(" << val_cpp << ");\n";
                            continue;
                        }
                    }
                }
                auto call_open = stmt.find('(');
                if (call_open != std::string::npos && stmt.back() == ')') {
                    std::string call_path = trim(stmt.substr(0, call_open));
                    auto call_dot = call_path.find('.');
                    if (call_dot != std::string::npos && call_path.find('.', call_dot + 1) == std::string::npos) {
                        std::string type_name = call_path.substr(0, call_dot);
                        std::string method_name = call_path.substr(call_dot + 1);
                        auto type_ns = g_type_ns.find(type_name);
                        std::string instance_type;
                        if (auto element = named_elements.find(type_name); element != named_elements.end())
                            instance_type = element->second;
                        if (instance_type.empty()) {
                            if (auto element = element_named_types.find(type_name); element != element_named_types.end())
                                instance_type = element->second;
                        }
                        if (instance_type.empty() && is_state_variable_name(type_name)) {
                            auto lower_name = type_name;
                            for (auto& ch : lower_name)
                                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                            for (auto const& state : view.state_vars) {
                                auto state_name = state.name;
                                for (auto& ch : state_name)
                                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                                if (state_name == lower_name) {
                                    instance_type = state.type;
                                    break;
                                }
                            }
                        }
                        if (instance_type.empty() && is_view_base_property_name(type_name))
                            instance_type = view.base_type;
                        auto generic = instance_type.find('<');
                        if (generic != std::string::npos) instance_type.resize(generic);
                        auto type_separator = instance_type.find_last_of(".:");
                        if (type_separator != std::string::npos)
                            instance_type = instance_type.substr(type_separator + 1);
                        for (auto& ch : instance_type)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        auto lower_method = method_name;
                        for (auto& ch : lower_method)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        auto methods = g_type_methods.find(instance_type);
                        if (is_ident(type_name) && is_ident(method_name)
                            && methods != g_type_methods.end() && methods->second.count(lower_method)) {
                            out << "        " << rewrite_callback_expr(stmt) << ";\n";
                            continue;
                        }
                        if (is_ident(type_name) && is_ident(method_name)
                            && is_view_base_property_name(type_name)) {
                            out << "        " << rewrite_callback_expr(stmt) << ";\n";
                            continue;
                        }
                        std::string static_type_key = type_name;
                        for (auto& ch : static_type_key)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        auto static_methods = g_type_static_methods.find(static_type_key);
                        bool metadata_static_method = static_methods != g_type_static_methods.end()
                            && static_methods->second.count(lower_method) != 0;
                        if (is_ident(type_name) && is_ident(method_name) && type_ns != g_type_ns.end()
                            && metadata_static_method && !named_elements.count(type_name)
                            && !element_named_types.count(type_name) && !is_state_variable_name(type_name)) {
                            std::string arguments = trim(stmt.substr(call_open + 1, stmt.size() - call_open - 2));
                            auto tag_expr_pos = arguments.find("(sender.Tag)");
                            if (tag_expr_pos != std::string::npos) {
                                std::size_t enum_end = tag_expr_pos;
                                while (enum_end > 0
                                    && (std::isalnum(static_cast<unsigned char>(arguments[enum_end - 1])) || arguments[enum_end - 1] == '_')) --enum_end;
                                std::string enum_name = arguments.substr(enum_end, tag_expr_pos - enum_end);
                                auto enum_type = g_enum_info.find(enum_name);
                                if (enum_type != g_enum_info.end()) {
                                    std::string tag_value = "winrt::unbox_value_or<winrt::hstring>(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(sender.get_abi()).Tag(), L\"0\")";
                                    std::string cast_expr = "static_cast<" + cpp_qualified(enum_type->second.first, enum_name)
                                        + ">(std::stoi(std::wstring(" + tag_value + ".to_view())))";
                                    arguments.replace(enum_end, tag_expr_pos + std::string("(sender.Tag)").size() - enum_end, cast_expr);
                                }
                            }
                            out << "        " << cpp_qualified(type_ns->second, type_name) << "::" << method_name
                                << "(" << rewrite_callback_expr(std::move(arguments)) << ");\n";
                            continue;
                        }
                    }
                }
                // Unsupported statement - preserve as a comment (still compile-safe)
                out << "        // " << stmt << "\n";
            }
        }
        // Component-view forwarding: if this handler corresponds to an event
        // other views attach to this view (Picker_ColorChanged fires
        // ColorChanged), raise the stored external handlers after the
        // component's own logic.
        for (auto const& ev : view.exposed_component_events)
        {
            std::string ev_clean = ev;
            if (ev_clean.size() > 2 && ev_clean[0] == 'o' && ev_clean[1] == 'n' && std::isupper(static_cast<unsigned char>(ev_clean[2])))
                ev_clean = ev_clean.substr(2);
            std::string ev_lo = ev_clean;
            for (auto& c : ev_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (cb.size() >= ev_clean.size()
                && std::tolower(static_cast<unsigned char>(cb[cb.size() - ev_clean.size()])) == std::tolower(static_cast<unsigned char>(ev_clean[0])))
            {
                std::string cb_tail = cb.substr(cb.size() - ev_clean.size());
                std::string ev_pascal = ev_clean;
                for (auto& c : cb_tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (auto& c : ev_pascal) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (cb_tail == ev_pascal)
                    out << "        _fire_" << ev_lo << "(sender, args);\n";
            }
        }
        out << "    }\n";
    }

    // Bare C# members referenced by translated handler bodies (BarItems,
    // ...): stub them as IInspectable members so the wiring compiles and
    // the host layer can populate them at runtime.
    for (auto const& m : extra_members)
        out << "    winrt::IInspectable " << m << ";\n";

    // Component-view event forwarding: pages attach .on<E> to instances of
    // this view (driver-aggregated in exposed_component_events). The class
    // gains public add_<E> accessors storing external handlers, and _fire_<E>
    // members that the matching internal handler calls after its own logic.
    if (!view.exposed_component_events.empty() && !is_app_class && !is_shell)
    {
        out << "\n    // external event forwarding (pages attach .on<E> to this view)\n";
        for (auto const& ev : view.exposed_component_events)
        {
            std::string ev_clean = ev;
            if (ev_clean.size() > 2 && ev_clean[0] == 'o' && ev_clean[1] == 'n' && std::isupper(static_cast<unsigned char>(ev_clean[2])))
                ev_clean = ev_clean.substr(2);
            std::string ev_lo = ev_clean;
            for (auto& c : ev_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            out << "    std::vector<std::function<void(IInspectable const&, IInspectable const&)>> m_" << ev_lo << "_handlers;\n";
            out << "public:\n";
            out << "    void add_" << ev_clean << "(std::function<void(IInspectable const&, IInspectable const&)> h) { m_" << ev_lo << "_handlers.push_back(h); }\n";
            if (ev != ev_clean)
                out << "    void add_" << ev << "(std::function<void(IInspectable const&, IInspectable const&)> h) { m_" << ev_lo << "_handlers.push_back(h); }\n";
            out << "private:\n";
            out << "    void _fire_" << ev_lo << "(IInspectable const& s, IInspectable const& e)\n";
            out << "    {\n";
            out << "        for (auto const& h : m_" << ev_lo << "_handlers) h(s, e);\n";
            out << "    }\n";
            out << "public:\n";
        }
    }

    // Close class definition (all classes)
    out << "};\n";

    // Window view classes export a launcher function. The class itself stays
    // module-local (the component module exports a same-named activation
    // wrapper struct, so exporting the class would collide). App::Start boots
    // the window through this function. The class derives from the projected
    // Window struct whose composable constructor creates the real runtime
    // window via IWindowFactory::CreateInstance.
    if (is_window_class && !is_shell
        && (view.main_window_class.empty() || view.name == view.main_window_class))
    {
        out << "\nexport " << cpp_class << "& get_" << class_name << "()\n";
        out << "{\n";
        out << "    static " << cpp_class << " window_view;\n";
        out << "    g_theme_state.get_main_window_control = +[](wchar_t const* control_type) -> void* {\n";
        out << "        if (!control_type) return nullptr;\n";
        out << "        std::wstring_view requested{control_type};\n";
        std::map<std::string, std::set<std::string>> control_members;
        for (auto const& [control, name] : view.main_window_controls)
            if (!control.empty() && !name.empty())
                control_members[control].insert(name);
        for (auto const& [control, members] : control_members)
        {
            if (members.size() != 1) continue;
            out << "        if (requested == L\"" << control << "\") return window_view."
                << *members.begin() << ".m_unknown.get();\n";
        }
        out << "        return nullptr;\n";
        out << "    };\n";
        out << "    return window_view;\n";
        out << "}\n";
        out << "\nexport void launch_" << class_name << "()\n";
        out << "{\n";
        out << "    winrt::boot_log(\"launch:begin\");\n";
        out << "    auto& window_view = get_" << class_name << "();\n";
        out << "    winrt::boot_log(\"launch:ctor-done\");\n";
        out << "    window_view.Activate();\n";
        out << "    winrt::boot_log(\"launch:activate-done\");\n";
        out << "}\n";
    }
    else if (is_shell)
    {
        // Shell: launch_<AppView>() boots the module-local window class. The
        // OnLaunched host calls exactly this launcher - the same contract as
        // a separate Window view, just from the App view's own module.
        out << "\nexport void launch_" << class_name << "()\n";
        out << "{\n";
        out << "    winrt::boot_log(\"launch:begin\");\n";
        out << "    static " << cpp_class << " window_view;\n";
        out << "    winrt::boot_log(\"launch:ctor-done\");\n";
        out << "    window_view.Activate();\n";
        out << "    winrt::boot_log(\"launch:activate-done\");\n";
        out << "}\n";
    }
    else if (!is_app_class && !is_shell)
    {
        // Page/UserControl views export a strongly-typed accessor returning
        // a reference to the page's singleton instance, and a factory returning
        // IInspectable. The single static instance is app-lifetime (same policy
        // as window_view): state survives back-and-forth navigation, the tree
        // is built once.
        out << "\nexport " << cpp_class << "& get_" << class_name << "()\n";
        out << "{\n";
        out << "    winrt::boot_log(\"get:" << class_name << "\");\n";
        out << "    static " << cpp_class << " instance;\n";
        out << "    winrt::boot_log(\"get-done:" << class_name << "\");\n";
        out << "    return instance;\n";
        out << "}\n";
        out << "\nexport winrt::IInspectable create_" << class_name << "()\n";
        out << "{\n";
        out << "    winrt::boot_log(\"create:" << class_name << "\");\n";
        out << "    auto* p = new " << cpp_class << "();\n";
        out << "    return winrt::IInspectable(*p);\n";
        out << "}\n";
        // Registration hook for the local activation registry (winrt_base):
        // main.ixx imports each view module and calls this once at startup so
        // winrt::<ns>::<Class>() wrappers resolve via the in-process factory
        // instead of RoActivateInstance (app classes have no registry entry,
        // and the exe exports nothing for DllGetActivationFactory).
        out << "\n// Register the local C++ factory under the runtime class name\n";
        out << "export void register_local_factory_" << class_name << "()\n";
        out << "{\n";
        out << "    winrt::impl::register_local_factory(L\"" << ns_meta << "." << class_name << "\", &create_" << class_name << ");\n";
        out << "}\n";
    }

    // Deterministic post-pass over the emitted module text: canonicalizes
    // enum spellings, boxes IInspectable-typed props, wraps Thickness values,
    // and comments x:Bind leftovers that reference elements this codegen can't
    // materialize. Shape-based only - never touches the prelude/helpers.
    std::string code = out.str();
    code = fixup_view_code(std::move(code), &g_type_events, &g_type_prop_kinds, &g_type_ns, &g_enum_info);
    return code;
}
