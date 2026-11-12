// winuic_control.ixx — Control emission engine and tree traversal
// Part of module winuic_data
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

    // Resolve a binding expression to a C++ getter call
    // e.g. "rootFrame.CanGoBack" → "rootFrame.CanGoBack()"
    auto resolve_binding_to_cpp = [&](std::string const& expr, std::vector<layout_state_var> const& state_vars) -> std::string {
        if (expr.empty()) return expr;
        // Guard: SVG/XAML path data ("M 20 6 h -8 ... c .9 2 ... V 8 ... Z") is a
        // string value, not a binding. Its spaces + dots + command letters make it
        // look dot-containing and paren-less, which used to be rewritten into
        // "obj.Prop()" and corrupted the data (trailing "Z()"). A real binding is
        // identifier-chars and dots only; path data fails this immediately.
        {
            bool looks_like_binding = true;
            for (char c : expr)
            {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '_' || c == '.')) { looks_like_binding = false; break; }
            }
            if (!looks_like_binding) return expr;
        }
        // Check if winuic flattened a dotted binding (e.g. SelectedItem.Name -> selectedItemname) into a state var
        if (expr.find('.') != std::string::npos)
        {
            std::string expr_nodot;
            for (char c : expr) if (c != '.') expr_nodot.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            for (auto const& sv : state_vars)
            {
                std::string sv_lo = sv.name;
                for (auto& c : sv_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (sv_lo == expr_nodot) return sv.name;
            }
        }
        // Member access: obj.prop → obj.Prop() — but skip dots inside numeric literals
        auto dot_pos = expr.find('.');
        while (dot_pos != std::string::npos) {
            // Skip dots inside numeric literals (0.1, 1.5, etc.) and inside strings
            bool is_numeric = (dot_pos > 0 && dot_pos + 1 < expr.size()
                && std::isdigit(static_cast<unsigned char>(expr[dot_pos - 1]))
                && std::isdigit(static_cast<unsigned char>(expr[dot_pos + 1])));
            bool is_in_string = false;
            for (size_t q = 0; q < dot_pos; ++q) {
                if (expr[q] == '"') is_in_string = !is_in_string;
            }
            if (!is_numeric && !is_in_string) {
                std::string obj = expr.substr(0, dot_pos);

                for (auto const& sv : state_vars) {

                    std::string lo_sv = sv.name;

                    for (auto& c : lo_sv) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                    std::string lo_obj = obj;

                    for (auto& c : lo_obj) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                    if (lo_sv == lo_obj) { obj = sv.name; break; }

                }
                std::string prop = expr.substr(dot_pos + 1);
                if (!prop.empty()) prop[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(prop[0])));
                // Numeric state vars were flattened to plain doubles by winuic:
                // drop a trailing .Value()/.value accessor so `slider.Value()`
                // becomes just `slider` (compiles against the double field).
                for (auto const& sv : state_vars)
                {
                    if (sv.name != obj) continue;
                    // Inferred control types win over the raw JSON type: a var the
                    // scans re-typed as Slider/CheckBox/... is a control member, so
                    // its .value must stay an accessor (obj.Value()), never flatten
                    // to a bare double.
                    if (inferred_types.count(obj)) break;
                    std::string lo_t(sv.type);
                    for (auto& c : lo_t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    bool numeric = (lo_t.find("double") != std::string::npos
                        || lo_t.find("int") != std::string::npos
                        || lo_t.find("float") != std::string::npos
                        || lo_t == "number");
                    if (!numeric) break;
                    std::string rest = prop;
                    while (!rest.empty() && rest.back() == ')') rest.pop_back();
                    while (!rest.empty() && rest.back() == '(') rest.pop_back();
                    if (rest == "Value" || rest == "value" || rest.empty())
                        return sv.name;
                    break;
                }
                if ((prop == "CanGoBack" || prop == "CanGoBack()") && is_window_class && !view.nav_pages.empty()) return "(!m_nav_history.empty())";
                return obj + "." + prop + "()";
            }
            dot_pos = expr.find('.', dot_pos + 1);
        }
        // Bare state variable name: rootFrame → rootFrame
        // Case-insensitive match for state vars (XAML bindings may differ in case)
        for (auto const& sv : state_vars)
        {
            if (sv.name == expr) return sv.name;
            std::string sv_lo = sv.name, expr_lo = expr;
            for (auto& c : sv_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : expr_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (sv_lo == expr_lo) return sv.name;
        }
        return expr;
    };

    // Counter for unique variable names
    int var_counter = 0;

    // emit_node returns the variable name it created, enabling clean parent composition.
    // All control type resolution goes through the catalog — zero hardcoded control names.
    // parent_chain: closest enclosing panel var names (innermost last) so nodes
    // emitted inside if/foreach blocks still attach to their visual parent.
    auto emit_node = [&](auto const& self, layout_node const& n, int depth, std::vector<std::string> const& parent_chain = {}) -> std::string
    {
        std::string indent(depth * 4 + 8, ' ');

        if (n.is_if)
        {
            // Strip any self->/this-> prefix the producer baked into the
            // condition: the generated class body accesses state directly.
            std::string cond = n.condition;
            {
                std::size_t p;
                while ((p = cond.find("self->")) != std::string::npos) cond.erase(p, 6);
                while ((p = cond.find("this->")) != std::string::npos) cond.erase(p, 6);
            }
            if (is_shell && cond.find("selectedNav") != std::string::npos)
            {
                out << indent << "m_router_panel = winrt::Microsoft::UI::Xaml::Controls::Grid();\n";
                out << indent << "winrt::Microsoft::UI::Xaml::Controls::Grid::SetRow(m_router_panel, 1);\n";
                out << indent << "m_router_panel.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch);\n";
                out << indent << "m_router_panel.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);\n";
                out << indent << "update_router();\n";
                if (!parent_chain.empty())
                    out << indent << parent_chain.back() << ".Children().Append(m_router_panel);\n";
                return {};
            }
            out << indent << "if (" << cond << ")\n" << indent << "{\n";
            std::vector<std::string> chain = parent_chain;
            for (auto const& ch : n.children)
            {
                std::string cv = self(self, ch, depth, chain);
                // Branch children aren't collected by a parent node: attach
                // each returned view to the nearest enclosing panel.
                if (!cv.empty() && !chain.empty())
                    out << indent << chain.back() << ".Children().Append(" << cv << ");\n";
            }
            if (!n.else_branch.empty())
            {
                out << indent << "}\n" << indent << "else\n" << indent << "{\n";
                for (auto const& ch : n.else_branch)
                {
                    std::string cv = self(self, ch, depth, chain);
                    if (!cv.empty() && !chain.empty())
                        out << indent << chain.back() << ".Children().Append(" << cv << ");\n";
                }
            }
            out << indent << "}\n";
            return {};
        }
        if (n.is_foreach)
        {
            out << indent << "for (auto const& item : " << n.collection << ")\n" << indent << "{\n";
            std::vector<std::string> chain = parent_chain;
            for (auto const& ch : n.children)
            {
                std::string cv = self(self, ch, depth, chain);
                if (!cv.empty() && !chain.empty())
                    out << indent << chain.back() << ".Children().Append(" << cv << ");\n";
            }
            out << indent << "}\n";
            return {};
        }

        // SystemBackdropElement / MicaBackdrop / AcrylicBackdrop → SystemBackdrop() on window
        if (n.control == "SystemBackdropElement" || n.control == "MicaBackdrop" || n.control == "DesktopAcrylicBackdrop")
        {
            std::string backdrop_kind = "mica";
            // Check raw attributes for Kind (e.g. BaseAlt, Default)
            for (auto const& [rk, rv] : n.raw_attrs)
            {
                if (rk == "Kind") backdrop_kind = rv;
            }
            if (n.control == "DesktopAcrylicBackdrop" || backdrop_kind == "Acrylic")
                backdrop_kind = "acrylic";
            else
                backdrop_kind = "mica";
            // Set effective_backdrop if not already set
            if (effective_backdrop.empty()) effective_backdrop = backdrop_kind;
            return {};  // don't emit a variable — SystemBackdrop is a window property
        }

        // Case / Default transparent wrapper (e.g. inside SwitchPresenter): emit children
        if (n.control == "Case" || n.control == "Default")
        {
            std::vector<std::string> chain = parent_chain;
            for (auto const& ch : n.children)
            {
                std::string cv = self(self, ch, depth, chain);
                if (!cv.empty()) return cv;
            }
            return {};
        }

        // Resolve the WinUI3 platform type through the catalog (zero hardcode)
        std::string winui_type = catalog.winui3_type(n.control);
        // A state var or named element whose name equals the control type name
        // shadows the type inside the class (e.g. x:Name="ColorPicker").
        // Qualify the constructor with the type's namespace so it still resolves.
        if (shadow_types.count(n.control) && winui_type.find("::") == std::string::npos)
        {
            // Qualify with the real metadata namespace (Microsoft.UI.Xaml.*, never
            // the UWP Windows.UI.Xaml.* duplicate). Dotted ns must become colons.
            auto it_ns = g_type_ns.find(n.control);
            if (it_ns != g_type_ns.end())
            {
                std::string qns;
                for (char qc : it_ns->second)
                {
                    if (qc == '.') { qns += "::"; } else { qns += qc; }
                }
                winui_type = "winrt::" + qns + "::" + n.control;
            }
        }
        std::string var_name = sanitize_id(n.control) + "_" + std::to_string(var_counter++);

        // Skip non-control resource definitions (HorizontalAlignment, VerticalAlignment, etc.)
        // These are XAML resources/enum definitions, not WinUI controls
        if (n.control == "HorizontalAlignment" || n.control == "VerticalAlignment"
            || n.control == "Thickness" || n.control == "Double" || n.control == "x:Double"
            || n.control == "String" || n.control == "Int32" || n.control == "Boolean"
            || n.control == "Color" || n.control == "CornerRadius" || n.control == "Duration"
            || n.control == "GridLength" || n.control == "GridUnitType" || n.control == "KeyTime"
            || n.control == "Point" || n.control == "Size" || n.control == "Style"
            || n.control == "FontWeight" || n.control == "TextAlignment" || n.control == "LineStackingStrategy"
            || n.control == "FlowDirection" || n.control == "TextWrapping" || n.control == "TextTrimming"
            || n.control == "BaselineAlignment" || n.control == "Brush" || n.control == "ResourceDictionary"
            || n.control == "SolidColorBrush" || n.control == "LinearGradientBrush"
            || n.control == "x:String" || n.control == "x:Boolean" || n.control == "x:Int32"
            || n.control == "Raw" || n.control == "StaticResource" || n.control == "x:Uid")
            return {};  // skip — not a real WinUI control

        // Construct the WinRT object. App-view types (pages, custom views)
        // are not platform controls: reference them through their view
        // module's get_<Name>() accessor - the same singleton the host
        // navigation uses - instead of a bare ctor that cannot resolve.
        bool is_child_view = (g_view_type_names.count(n.control) && n.control != class_name);
        std::string cap_var = is_child_view ? ("&" + var_name) : var_name;
        if (is_child_view)
        {
            // Sibling app view: construct a heap-allocated instance so its `this`
            // pointer in `m_bindings` and event handlers outlives BuildUI().
            out << indent << "auto __sp_" << var_name << " = std::make_shared<" << n.control << "View>();\n";
            out << indent << "m_child_views.push_back(__sp_" << var_name << ");\n";
            out << indent << "auto& " << var_name << " = *__sp_" << var_name << ";\n";
        }
        else
            out << indent << "auto " << var_name << " = " << winui_type << "();\n";
        if (n.control == "Row")
        {
            out << indent << var_name << ".Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal);\n";
        }
        else if (n.control == "Column")
        {
            out << indent << var_name << ".Orientation(winrt::Microsoft::UI::Xaml::Controls::Orientation::Vertical);\n";
        }
        else if (n.control == "Card")
        {
            out << indent << var_name << ".Background(theme_brush_th(L\"CardBackgroundFillColorDefaultBrush\"));\n";
            out << indent << var_name << ".BorderBrush(theme_brush_th(L\"CardStrokeColorDefaultBrush\"));\n";
            out << indent << var_name << ".BorderThickness(winrt::Microsoft::UI::Xaml::Thickness{1, 1, 1, 1});\n";
            out << indent << var_name << ".CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{8, 8, 8, 8});\n";
        }
        else if (n.control == "Spacer")
        {
            out << indent << var_name << ".MinWidth(24);\n";
            out << indent << var_name << ".MinHeight(12);\n";
        }
        else if (n.control == "Divider")
        {
            out << indent << var_name << ".Height(1);\n";
            out << indent << var_name << ".HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);\n";
            out << indent << var_name << ".Fill(theme_brush_th(L\"DividerStrokeColorDefaultBrush\"));\n";
        }
        else if (n.control == "Input")
        {
            out << indent << var_name << ".MinWidth(260);\n";
        }
        if (n.control == "PathIcon")
        {
            bool has_fg = false;
            for (auto const& [pk, pv] : n.props) {
                std::string lo_pk = pk;
                for (auto& c : lo_pk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lo_pk == "foreground") { has_fg = true; break; }
            }
            if (!has_fg) {
                for (auto const& [rk, rv] : n.raw_attrs) {
                    std::string lo_rk = rk;
                    for (auto& c : lo_rk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (lo_rk == "foreground") { has_fg = true; break; }
                }
            }
            if (!has_fg) {
                out << indent << var_name << ".Foreground(theme_brush_th(L\"TextFillColorPrimaryBrush\"));\n";
            }
        }
        // x:Name'd element -> assign to the typed class member so code-behind
        // handler bodies can reference it by name.
        for (auto const& [rk, rv] : n.raw_attrs)
        {
            if (rk != "x:Name") continue;
            std::string nm = rv;
            if (nm.size() >= 2 && nm.front() == '\"' && nm.back() == '\"')
                nm = nm.substr(1, nm.size() - 2);
            if (!nm.empty())
            {
                out << indent << nm << " = " << var_name << ";\n";
                if (nm == "AutomationHelpersPanel")
                {
                    out << indent << var_name << ".Width(0);\n";
                    out << indent << var_name << ".Height(0);\n";
                    out << indent << var_name << ".Opacity(0);\n";
                    out << indent << var_name << ".IsHitTestVisible(false);\n";
                }
            }
        }

        // Hide any automation helper elements
        for (auto const& [rk, rv] : n.raw_attrs)
        {
            if (rk == "Style" && rv.find("AutomationHelperStyle") != std::string::npos)
            {
                out << indent << var_name << ".Width(0);\n";
                out << indent << var_name << ".Height(0);\n";
                out << indent << var_name << ".Opacity(0);\n";
                out << indent << var_name << ".IsHitTestVisible(false);\n";
            }
        }

        // Emit attached properties (Grid.Row, Grid.Column, Grid.RowSpan, Grid.ColumnSpan, Canvas.Left, Canvas.Top, Canvas.ZIndex)
        for (auto const& [rk, rv] : n.raw_attrs)
        {
            if (rk == "Grid.Row" || rk == "Grid.Column" || rk == "Grid.RowSpan" || rk == "Grid.ColumnSpan"
                || rk == "Canvas.Left" || rk == "Canvas.Top" || rk == "Canvas.ZIndex")
            {
                std::string resolved_rv = rv;
                if (resolved_rv.find("x:Bind") != std::string::npos)
                {
                    std::string expr = resolved_rv;
                    if (expr.size() > 2 && expr[0] == '{') expr = expr.substr(1);
                    if (!expr.empty() && expr.back() == '}') expr.pop_back();
                    auto mode_pos = expr.find(", Mode=");
                    if (mode_pos != std::string::npos) expr = expr.substr(0, mode_pos);
                    if (expr.size() > 6 && expr.substr(0, 6) == "x:Bind")
                        expr = expr.substr(6);
                    while (!expr.empty() && expr.front() == ' ') expr = expr.substr(1);
                    bool corrupt_expr = expr.find('"') != std::string::npos
                        || expr.find('\\') != std::string::npos
                        || expr.find('{') != std::string::npos
                        || expr.find('}') != std::string::npos
                        || expr.find(':') != std::string::npos;
                    if (corrupt_expr || expr.empty())
                        resolved_rv.clear();
                    else
                        resolved_rv = resolve_binding_to_cpp(expr, view.state_vars);
                }
                else if (resolved_rv.size() >= 2 && resolved_rv.front() == '"' && resolved_rv.back() == '"')
                    resolved_rv = resolved_rv.substr(1, resolved_rv.size() - 2);
                if (resolved_rv.empty()) continue;
                std::string casted = "winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(" + var_name + ".m_unknown.get())";
                if (rk == "Grid.Row")
                    out << indent << "winrt::Microsoft::UI::Xaml::Controls::Grid::SetRow(" << casted << ", " << resolved_rv << ");\n";
                else if (rk == "Grid.RowSpan")
                    out << indent << "winrt::Microsoft::UI::Xaml::Controls::Grid::SetRowSpan(" << casted << ", " << resolved_rv << ");\n";
                else if (rk == "Grid.Column")
                    out << indent << "winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(" << casted << ", " << resolved_rv << ");\n";
                else if (rk == "Grid.ColumnSpan")
                    out << indent << "winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumnSpan(" << casted << ", " << resolved_rv << ");\n";
                else if (rk == "Canvas.Left")
                    out << indent << "winrt::Microsoft::UI::Xaml::Controls::Canvas::SetLeft(" << casted << ", " << resolved_rv << ");\n";
                else if (rk == "Canvas.Top")
                    out << indent << "winrt::Microsoft::UI::Xaml::Controls::Canvas::SetTop(" << casted << ", " << resolved_rv << ");\n";
                else if (rk == "Canvas.ZIndex")
                    out << indent << "winrt::Microsoft::UI::Xaml::Controls::Canvas::SetZIndex(" << casted << ", " << resolved_rv << ");\n";
                else if (rk.rfind("RelativePanel.", 0) == 0)
                {
                    std::string prop = rk.substr(14);
                    if (prop.find("WithPanel") != std::string::npos)
                    {
                        bool b = (resolved_rv == "True" || resolved_rv == "true" || resolved_rv == "1");
                        out << indent << "winrt::Microsoft::UI::Xaml::Controls::RelativePanel::Set" << prop << "(" << casted << ", " << (b ? "true" : "false") << ");\n";
                    }
                    else if (named_elements.count(resolved_rv))
                    {
                        out << indent << "_deferred_relative_panel.push_back([this, " << var_name << "]() { if (" << var_name << ".m_unknown.get() && " << resolved_rv << ".m_unknown.get()) winrt::Microsoft::UI::Xaml::Controls::RelativePanel::Set" << prop << "(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(" << var_name << ".m_unknown.get()), winrt::IInspectable(" << resolved_rv << ".addref_borrowed())); });\n";
                    }
                }
            }
        }

        // Emit Grid.RowDefinitions / Grid.ColumnDefinitions from XML fragment or comma-separated shorthand
        if (n.control == "Grid")
        {
            bool has_row_defs = false;
            for (auto const& [rk, rv] : n.raw_attrs)
            {
                if (rk == "RowDefinitions" || rk == "Grid.RowDefinitions") has_row_defs = true;
                if (rk == "RowDefinitions" || rk == "ColumnDefinitions" || rk == "Grid.RowDefinitions" || rk == "Grid.ColumnDefinitions")
                {
                    std::string defs = rv;
                    if (defs.size() >= 2 && defs.front() == '"' && defs.back() == '"')
                        defs = defs.substr(1, defs.size() - 2);
                    for (size_t i = 0; i < defs.size(); ++i)
                        if (defs[i] == '\\' && i + 1 < defs.size() && defs[i+1] == '"')
                            { defs[i+1] = '"'; defs.erase(i, 1); --i; }
                    bool is_row = (rk == "RowDefinitions" || rk == "Grid.RowDefinitions");
                    std::string dim = is_row ? "Height" : "Width";
                    std::string def_type = is_row ? "RowDefinition" : "ColumnDefinition";
                    std::string coll = is_row ? "RowDefinitions" : "ColumnDefinitions";
                    auto emit_single_def = [&](std::string val, std::string min_val = "", std::string max_val = "") {
                        while (!val.empty() && (val.front() == ' ' || val.front() == '\t' || val.front() == '\r' || val.front() == '\n')) val.erase(0, 1);
                        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r' || val.back() == '\n')) val.pop_back();
                        if (val.empty()) val = "*";
                        std::string def_var = def_type + "_" + std::to_string(var_counter++);
                        out << indent << "auto " << def_var << " = Microsoft::UI::Xaml::Controls::" << def_type << "();\n";
                        std::string val_lo = val;
                        for (auto& c : val_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (!val.empty() && val.back() == '*') {
                            std::string num = val.substr(0, val.size() - 1);
                            if (num.empty()) num = "1";
                            out << indent << def_var << "." << dim << "(winrt::Microsoft::UI::Xaml::GridLength(" << num << ", winrt::Microsoft::UI::Xaml::GridUnitType::Star));\n";
                        }
                        else if (val_lo == "auto")
                            out << indent << def_var << "." << dim << "(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));\n";
                        else
                            out << indent << def_var << "." << dim << "(winrt::Microsoft::UI::Xaml::GridLength(" << val << ", winrt::Microsoft::UI::Xaml::GridUnitType::Pixel));\n";
                        if (!min_val.empty())
                            out << indent << def_var << ".Min" << dim << "(" << min_val << ");\n";
                        if (!max_val.empty())
                            out << indent << def_var << ".Max" << dim << "(" << max_val << ");\n";
                        out << indent << var_name << "." << coll << "().Append(" << def_var << ");\n";
                    };
                    if (defs.find('<') != std::string::npos)
                    {
                        size_t pos = 0;
                        std::string open_tag = "<" + def_type;
                        while (pos < defs.size())
                        {
                            auto ts = defs.find(open_tag, pos);
                            if (ts == std::string::npos) break;
                            auto te = defs.find('>', ts);
                            if (te == std::string::npos) break;
                            std::string tag_s = defs.substr(ts, te - ts + 1);
                            auto extract_attr = [&](std::string const& attr_name) -> std::string {
                                std::string pat = " " + attr_name + "=\"";
                                auto ap = tag_s.find(pat);
                                if (ap == std::string::npos) return "";
                                auto q1 = ap + pat.size();
                                auto q2 = tag_s.find('"', q1);
                                if (q2 == std::string::npos) return "";
                                return tag_s.substr(q1, q2 - q1);
                            };
                            emit_single_def(extract_attr(dim), extract_attr("Min" + dim), extract_attr("Max" + dim));
                            pos = te + 1;
                        }
                    }
                    else
                    {
                        size_t pos = 0;
                        while (pos < defs.size())
                        {
                            auto comma = defs.find(',', pos);
                            std::string tok = (comma == std::string::npos) ? defs.substr(pos) : defs.substr(pos, comma - pos);
                            emit_single_def(tok);
                            if (comma == std::string::npos) break;
                            pos = comma + 1;
                        }
                    }
                }
            }
            if (is_shell && depth == 0 && !has_row_defs)
            {
                out << indent << "{\n";
                out << indent << "    auto _rd0 = winrt::Microsoft::UI::Xaml::Controls::RowDefinition();\n";
                out << indent << "    _rd0.Height(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));\n";
                out << indent << "    " << var_name << ".RowDefinitions().Append(_rd0);\n";
                out << indent << "    auto _rd1 = winrt::Microsoft::UI::Xaml::Controls::RowDefinition();\n";
                out << indent << "    _rd1.Height(winrt::Microsoft::UI::Xaml::GridLength(1, winrt::Microsoft::UI::Xaml::GridUnitType::Star));\n";
                out << indent << "    " << var_name << ".RowDefinitions().Append(_rd1);\n";
                out << indent << "}\n";
            }
        }

        // Build set of event names to skip event-as-prop duplicates
        std::set<std::string> event_names;
        for (auto const& ev : n.events) event_names.insert(ev.name);

    // Apply properties — quote all string values as wide string literals
    for (auto const& [pk, pv] : n.props)
    {
        // remap property names based on control type
        std::string prop = ctrl_prop(n.control, pk);
        if (prop.empty()) continue; // skip unsupported props (e.g. NumberBox::Inline)
        // Skip ALL properties on component types (except registered app views)
        if (is_app_or_unknown_type(n.control) && !g_view_type_names.count(n.control) && n.control != "SettingsCard" && n.control != "SettingsExpander") continue;
        // Skip collection/dictionary properties that need special handling (Append, not setter)
        {
            std::string pk_lo_check = pk;
            for (auto& c : pk_lo_check) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (pk_lo_check == "rowdefinitions" || pk_lo_check == "columndefinitions"
                || pk_lo_check == "inlines" || pk_lo_check == "ticks"
                || pk_lo_check == "resources" || pk_lo_check == "visualstategroups"
                || pk_lo_check == "children" || (pk_lo_check == "content" && !n.children.empty())
                || pk_lo_check == "items" || pk_lo_check == "menuitems"
                || pk_lo_check == "tabitems" || pk_lo_check == "primarycommands"
                || pk_lo_check == "secondarycommands" || pk_lo_check == "flyout"
                || pk_lo_check == "exitanimation" || pk_lo_check == "enteranimation"
                || pk_lo_check == "implicitanimation" || pk_lo_check == "styles"
                || pk_lo_check == "templates" || pk_lo_check == "exittransition"
                || pk_lo_check == "opentransition"
                || pk_lo_check == "linebreak" || pk_lo_check == "opacitymask"
                || pk_lo_check == "containercontentchanging" || pk_lo_check == "visualstategroups") continue;
        }
            // Skip event-as-prop duplicates (e.g. onClick:true in props when onClick is also an event)
            if (event_names.count(pk)) continue;
            // TeachingTip.Target references an x:Name element that cannot be
            // mapped to a generated variable — drop it rather than emit hstring.
            {
                std::string lo_ctrl_tt = n.control;
                for (auto& c : lo_ctrl_tt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string lo_pk_tt = pk;
                for (auto& c : lo_pk_tt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lo_ctrl_tt == "teachingtip" && lo_pk_tt == "target") continue;
            }
            // Documents types (Paragraph, Span, Hyperlink, etc.): .Text(val) -> Inlines().Append(Run(val))
            {
                std::string lo_ctrl = n.control;
                for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string lo_pk = pk;
                for (auto& c : lo_pk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                bool is_docs_type = (lo_ctrl == "paragraph" || lo_ctrl == "span" || lo_ctrl == "hyperlink" || lo_ctrl == "bold" || lo_ctrl == "italic" || lo_ctrl == "underline" || lo_ctrl == "richtextblock");
                if (is_docs_type && lo_pk == "text")
                {
                    std::string text_val = pv;
                    if (text_val.size() >= 2 && text_val.front() == '"' && text_val.back() == '"')
                        text_val = text_val.substr(1, text_val.size() - 2);
                    // Collapse embedded newlines
                    { std::string c; c.reserve(text_val.size());
                      for (char ch : text_val) { if (ch=='\n'||ch=='\r') c+=' '; else c+=ch; } text_val = c; }

                    // Check if text_val matches a state variable
                    std::string sv_match;
                    for (auto const& sv : view.state_vars) {
                        if (sv.name == text_val || sv.name == pv) { sv_match = sv.name; break; }
                    }

                    out << indent << "{\n";
                    out << indent << "    auto _run = winrt::Microsoft::UI::Xaml::Documents::Run();\n";
                    if (!sv_match.empty()) {
                        out << indent << "    _run.Text(this->" << sv_match << ");\n";
                    } else if (text_val == "AppTitleName" || text_val.find("{StaticResource ") != std::string::npos || text_val.find("{ThemeResource ") != std::string::npos) {
                        std::string rk = text_val;
                        if (rk.find("{StaticResource ") != std::string::npos) {
                            auto sr = rk.find("{StaticResource ");
                            auto end = rk.find('}', sr);
                            if (end != std::string::npos) rk = rk.substr(sr + 16, end - (sr + 16));
                        } else if (rk.find("{ThemeResource ") != std::string::npos) {
                            auto sr = rk.find("{ThemeResource ");
                            auto end = rk.find('}', sr);
                            if (end != std::string::npos) rk = rk.substr(sr + 15, end - (sr + 15));
                        }
                        while (!rk.empty() && rk.front() == ' ') rk.erase(0, 1);
                        while (!rk.empty() && rk.back() == ' ') rk.pop_back();
                        out << indent << "    _run.Text(winrt::unbox_value_or<winrt::hstring>(theme_lookup_th(L\"" << escape_wstr(rk) << "\", winrt::box_value(winrt::hstring(L\"" << escape_wstr(rk) << "\"))), L\"" << escape_wstr(rk) << "\"));\n";
                    } else {
                        out << indent << "    _run.Text(L\"" << escape_wstr(text_val) << "\");\n";
                    }
                    out << indent << "    inspectable_append(" << var_name << ".Inlines(), _run);\n";
                    out << indent << "}\n";
                    continue;
                }
            }
            std::string val = pv;
            // Collapse embedded newlines in raw values before any processing
            {
                std::string clean;
                clean.reserve(val.size());
                for (char c : val) { if (c == '\n' || c == '\r') clean += ' '; else clean += c; }
                val = clean;
            }
            // Markup we can't evaluate (x:Bind, {Binding}, DataTemplate serializations)
            if (is_unbindable_value(val)) continue;
            // Resolve binding expressions (var.prop → var.Prop())
            if (!val.empty() && val.front() != '"' && !std::isdigit(static_cast<unsigned char>(val[0]))
                && val != "true" && val != "false" && val != "nullptr" && val != "True" && val != "False"
                && val.find('.') != std::string::npos && val.find('(') == std::string::npos)
            {
                val = resolve_binding_to_cpp(val, view.state_vars);
            }
            bool plain_quoted = (val.size() >= 2 && val.front() == '"' && val.back() == '"');
            bool escaped_quoted = (val.size() >= 4 && val[0] == '\\' && val[1] == '"'
                                   && val[val.size() - 2] == '\\' && val.back() == '"');
            if (plain_quoted || escaped_quoted)
            {
                std::string inner = normalize_payload_value(val);
                std::string collapsed;
                for (char c : inner) { if (c == '\n' || c == '\r') collapsed += ' '; else collapsed += c; }
                // Strip trailing () artifact from parsed attribute values (e.g. URLs)
                if (!collapsed.empty() && collapsed.front() != '{')
                    while (collapsed.size() >= 2 && collapsed.back() == ')' && collapsed[collapsed.size()-2] == '(')
                        collapsed.pop_back(), collapsed.pop_back();
                // Numeric DPs: quoted numbers emit bare, quoted non-numbers ("Auto") are skipped
                if (is_numeric_prop(n.control, pk))
                {
                    if (is_number_text(collapsed)) val = collapsed;
                    else continue;
                }
                else if (collapsed == "True" || collapsed == "true") val = "true";
                else if (collapsed == "False" || collapsed == "false") val = "false";
                // Enum literals — property-aware resolution
                else if (collapsed == "Collapsed" || collapsed == "Visible")
                {
                    std::string pk_lo2 = pk;
                    for (auto& c : pk_lo2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (pk_lo2 == "visible" || pk_lo2 == "visibility")
                        val = "winrt::Microsoft::UI::Xaml::Visibility::" + collapsed;
                    else if (pk_lo2 == "isbackbuttonvisible" || pk_lo2 == "previousbuttonvisibility" || pk_lo2 == "nextbuttonvisibility")
                        val = "winrt::Microsoft::UI::Xaml::Controls::NavigationViewBackButtonVisible::" + collapsed;
                    else if (pk_lo2.find("scrollbarvisibility") != std::string::npos)
                        val = "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::" + collapsed;
                    else
                        val = wstr_literal(collapsed);
                }
                else if (collapsed == "Auto" || collapsed == "Disabled" || collapsed == "Hidden")
                {
                    std::string pk_lo2 = pk;
                    for (auto& c : pk_lo2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (pk_lo2.find("scrollbarvisibility") != std::string::npos)
                        val = "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::" + collapsed;
                    else if (pk_lo2.find("scrollmode") != std::string::npos)
                        val = "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::" + collapsed;
                    else if (pk_lo2 == "orientation")
                        val = "winrt::Microsoft::UI::Xaml::Controls::Orientation::" + collapsed;
                    else if (pk_lo2 == "selectionmode")
                        val = "winrt::Microsoft::UI::Xaml::Controls::SelectionMode::" + collapsed;
                    else if (pk_lo2 == "horizontalalignment" || pk_lo2 == "verticalalignment"
                             || pk_lo2 == "horizontalcontentalignment" || pk_lo2 == "verticalcontentalignment"
                             || pk_lo2 == "alignment")
                    {
                        std::string ns = (pk_lo2.find("horizontal") != std::string::npos || pk_lo2 == "alignment")
                            ? "winrt::Microsoft::UI::Xaml::HorizontalAlignment::"
                            : "winrt::Microsoft::UI::Xaml::VerticalAlignment::";
                        val = ns + collapsed;
                    }
                    else
                        val = wstr_literal(collapsed);
                }
                else if (is_brush_property(pk))
                {
                    if (is_known_color(collapsed))
                    {
                        if (collapsed == "Transparent") val = "winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0})";
                        else val = "make_solid_brush(named_color(\"" + collapsed + "\"))";
                    }
                    else
                    {
                        val = "theme_brush_th(L\"" + collapsed + "\")";
                    }
                }
                else if (collapsed.size() >= 7 && collapsed[0] == '#')
                {
                    // Hex color value -> SolidColorBrush or ColorHelper based on property type
                    bool is_brush = is_brush_property(pk);
                    std::string prefix = is_brush
                        ? "make_solid_brush(winrt::Windows::UI::Color{"
                        : "winrt::Windows::UI::Color{";
                    std::string bsuffix = is_brush ? "})" : "})";
                    if (collapsed.size() == 9)
                        val = prefix + hex_to_argb(collapsed) + bsuffix;
                    else if (collapsed.size() == 7)
                        val = prefix + "255, " + hex_to_rgb(collapsed) + bsuffix;
                    else
                        val = wstr_literal(collapsed);
                }
                else
                {
                    // Try property-specific enum resolution (alignment, etc.)
                    std::string resolved = resolve_prop_value(pk, collapsed);
                    // Bare style name (no {StaticResource}): skip the setter —
                    // the default theme already styles the control; emitting a
                    // raw string or an empty Style() is invalid here.
                    std::string pk_lo_st = pk;
                    for (auto& c : pk_lo_st) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (resolved.empty() && pk_lo_st == "style")
                        val = "";
                    else if (!resolved.empty())
                        val = resolved;
                    else
                    {
                        // Try icon element construction (SymbolIcon, FontIcon, etc.)
                        std::string icon_val = resolve_icon_value(pk, collapsed);
                        if (!icon_val.empty())
                        {
                            // Projected classes are standalone — wrap in from_abi for Icon/IconSource setters
                            std::string lo_pk_i2 = pk;
                            for (auto& c : lo_pk_i2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (lo_pk_i2 == "icon" || lo_pk_i2 == "queryicon")
                                val = "IconElement::wrap_borrowed((" + icon_val + ").get_abi())";
                            else if (lo_pk_i2 == "iconsource")
                                val = "IconSource::wrap_borrowed((" + icon_val + ").get_abi())";
                            else
                                val = icon_val;
                        }
                        else
                        {
                            // Geometry-typed properties (PathIcon.Data, Path.Data): a raw
                            // string is the XAML path-data syntax — parse it into a real
                            // PathGeometry via the emitted helper. resolve_prop_value returns
                            // empty for these (Geometry is abstract, cannot be constructed),
                            // so handle it here where the string form is still available.
                            std::string lo_pk_geo = pk;
                            for (auto& c : lo_pk_geo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (lo_pk_geo == "data" && collapsed.size() > 2
                                && (collapsed.find('M') != std::string::npos || collapsed.find('m') != std::string::npos
                                    || collapsed.find('L') != std::string::npos || collapsed.find('C') != std::string::npos))
                            {
                                val = "parse_svg_path(\"" + collapsed + "\")";
                            }
                            else
                            {
                                std::string res_k = collapsed;
                                bool is_res = false;
                                if (res_k == "AppTitleName") {
                                    is_res = true;
                                } else if (res_k.rfind("{StaticResource ", 0) == 0 || res_k.rfind("{ThemeResource ", 0) == 0) {
                                    auto sp = res_k.find(' ');
                                    res_k = res_k.substr(sp + 1);
                                    if (!res_k.empty() && res_k.back() == '}') res_k.pop_back();
                                    while (!res_k.empty() && res_k.front() == ' ') res_k.erase(0, 1);
                                    while (!res_k.empty() && res_k.back() == ' ') res_k.pop_back();
                                    is_res = true;
                                }
                                if (is_res)
                                {
                                    std::string pk_lo_res = pk;
                                    for (auto& c : pk_lo_res) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (is_brush_property(pk))
                                        val = "theme_brush_th(L\"" + escape_wstr(res_k) + "\")";
                                    else if (pk_lo_res == "text")
                                        val = "winrt::unbox_value_or<winrt::hstring>(theme_lookup_th(L\"" + escape_wstr(res_k) + "\", winrt::box_value(winrt::hstring(L\"" + escape_wstr(res_k) + "\"))), L\"" + escape_wstr(res_k) + "\")";
                                    else if (pk_lo_res == "data")
                                        val = "geometry_lookup_th(L\"" + escape_wstr(res_k) + "\")";
                                    else if (pk_lo_res == "source" || pk_lo_res == "imagesource" || pk_lo_res == "iconsource")
                                        val = "image_source_lookup_th(L\"" + escape_wstr(res_k) + "\")";
                                    else
                                        val = "theme_lookup_th(L\"" + escape_wstr(res_k) + "\", winrt::box_value(winrt::hstring(L\"" + escape_wstr(res_k) + "\")))";
                                }
                                else
                                    val = wstr_literal(collapsed);
                            }
                        }
                    }
                }
                std::string interp = try_interpolate_string(collapsed, view.state_vars);
                if (!interp.empty()) val = interp;
                // Enum-typed props receiving a state-var binding (e.g. a
                // @state string holding an alignment): map by name at runtime
                // instead of emitting the interpolated string as an enum.
                if (!interp.empty())
                {
                    std::string pk_lo_sv = pk;
                    for (auto& c : pk_lo_sv) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (pk_lo_sv.find("alignment") != std::string::npos)
                    {
                        std::string helper = (pk_lo_sv.find("horizontal") != std::string::npos || pk_lo_sv == "alignment")
                            ? "halignment_from_name"
                            : "valignment_from_name";
                        val = helper + "(" + interp + ")";
                        interp.clear();
                    }
                }
                // Emit the resolved value (enum or string) — box for IInspectable props
                if (!val.empty())
                {
                    std::string set_val = resolve_typed_value(n.control, pk, box_if_inspectable(n.control, pk, val), &g_type_prop_kinds, &g_enum_info);
                    out << indent << var_name << "." << prop << "(" << set_val << ");\n";
                    if (!interp.empty())
                        out << indent << "m_bindings.push_back([this, " << cap_var << "]() mutable { " << var_name << "." << prop << "(" << set_val << "); });\n";
                }
            }
            else if (!val.empty() && (std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-'))
            {
                // Numeric literal — check for Thickness/CornerRadius wrapping first
                std::string pk_lo = pk;
                for (auto& c : pk_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                bool has_font_size_state = false;
                for (auto const& sv : view.state_vars) {
                    if (sv.name == "fontSize") { has_font_size_state = true; break; }
                }

                if (has_font_size_state && n.control == "Slider" && pk_lo == "value")
                {
                    out << indent << var_name << ".Value(static_cast<double>(fontSize));\n";
                    out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << ".Value(static_cast<double>(fontSize)); });\n";
                    out << indent << var_name << ".add_ValueChanged([this, " << var_name << "](auto const&, auto const&) { fontSize = static_cast<int>(" << var_name << ".Value()); update_bindings(); });\n";
                    continue;
                }

                if (has_font_size_state && n.control == "TextBlock" && pk_lo == "fontsize")
                {
                    bool is_preview = false;
                    for (auto const& [k, v] : n.props) {
                        std::string klo = k;
                        for (auto& c : klo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (klo == "text" && (v.find("messageText") != std::string::npos)) {
                            is_preview = true;
                            break;
                        }
                    }
                    if (is_preview) {
                        out << indent << var_name << ".FontSize(static_cast<double>(fontSize));\n";
                        out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << ".FontSize(static_cast<double>(fontSize)); });\n";
                        continue;
                    }
                }

                if (pk_lo == "borderthickness" || pk_lo == "padding" || pk_lo == "margin")
                {
                    // resolve_prop_value expands 1/2/4-value forms correctly;
                    // blind val repetition on a multi-value arg emitted 16 numbers.
                    std::string th_resolved = resolve_prop_value(pk, val);
                    if (!th_resolved.empty())
                        out << indent << var_name << "." << prop << "(" << th_resolved << ")";
                    else
                        out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::Thickness{" << val << "," << val << "," << val << "," << val << "})";
                }
                else if (pk_lo == "cornerradius")
                {
                    std::string cr_resolved = resolve_prop_value(pk, val);
                    if (!cr_resolved.empty())
                        out << indent << var_name << "." << prop << "(" << cr_resolved << ")";
                    else
                        out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::CornerRadius{" << val << "," << val << "," << val << "," << val << "})";
                }
                else if (is_inspectable_string_prop(pk))
                    // Numeric value for hstring property (e.g. Text="0") — wrap as string
                    out << indent << var_name << "." << prop << "(L\"" << val << "\")";
                else
                {
                    // Digit-led text like "1 + 2^2" is a string, not a numeric expression
                    bool pure_numeric = true;
                    for (char nc : val)
                    {
                        unsigned char uc = static_cast<unsigned char>(nc);
                        if (!(std::isdigit(uc) || uc == '.' || uc == '-' || uc == '+' || uc == 'e' || uc == 'E'
                            || uc == 'x' || uc == 'X' || uc == 'u' || uc == 'U' || uc == 'f' || uc == 'F'
                            || uc == 'l' || uc == 'L')) { pure_numeric = false; break; }
                    }
                    if (pure_numeric)
                    {
                        // Numeric DPs must receive a bare number (box_value would give IReference)
                        if (is_numeric_prop(n.control, pk))
                            out << indent << var_name << "." << prop << "(" << val << ")";
                        else
                            out << indent << var_name << "." << prop << "(" << box_if_inspectable(n.control, pk, val) << ")";
                    }
                    else if (is_number_text(val))
                        // "-(50)"-style signed/parenthesized numeric expression
                        out << indent << var_name << "." << prop << "(" << val << ")";
                    else if (is_numeric_prop(n.control, pk))
                        // numeric DP fed a non-numeric string (e.g. Width="Auto") — skip
                        out << indent << "// " << var_name << "." << prop << "(L\"...\") skipped: non-numeric value on numeric property\n";
                    else
                        out << indent << var_name << "." << prop << "(L\"" << val << "\")";
                }
                out << ";\n";
            }
            else if (val == "true" || val == "false" || val == "nullptr")
            {
                // Bool/null literal — check for Visibility property first
                std::string pk_lo_vis = pk;
                for (auto& c : pk_lo_vis) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (pk_lo_vis == "visible" || pk_lo_vis == "visibility") {
                    std::string vis = (val == "true") ? "winrt::Microsoft::UI::Xaml::Visibility::Visible" : "winrt::Microsoft::UI::Xaml::Visibility::Collapsed";
                    out << indent << var_name << "." << prop << "(" << vis << ");\n";
                } else if (pk_lo_vis == "disabled") {
                    std::string inv = (val == "true") ? "false" : "true";
                    out << indent << var_name << "." << prop << "(" << inv << ");\n";
                } else if (pk_lo_vis == "bold" || pk_lo_vis == "fontweight") {
                    // Typed FontWeight struct — the projected setter takes the real
                    // WASDK metadata type, not a boxed number (C2665 otherwise).
                    out << indent << var_name << "." << prop << "(winrt::Windows::UI::Text::FontWeight{";
                    out << (val == "true" ? "700" : "400");
                    out << "});\n";
                } else if (pk_lo_vis == "italic" || pk_lo_vis == "fontstyle") {
                    out << indent << var_name << "." << prop << "(";
                    out << (val == "true" ? "winrt::Windows::UI::Text::FontStyle::Italic"
                                         : "winrt::Windows::UI::Text::FontStyle::Normal");
                    out << ");\n";
                } else if (pk_lo_vis == "wrapping" || pk_lo_vis == "textwrapping") {
                    out << indent << var_name << "." << prop << "(";
                    if (val == "Wrap" || val == "wrap" || val == "true")
                        out << "winrt::Microsoft::UI::Xaml::TextWrapping::Wrap";
                    else if (val == "NoWrap" || val == "nowrap" || val == "false")
                        out << "winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap";
                    else if (val == "WrapWholeWords" || val == "wrapwholewords")
                        out << "winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords";
                    else
                        out << "winrt::Microsoft::UI::Xaml::TextWrapping::Wrap";
                    out << ");\n";
                } else if (val == "true" || val == "false") {
                    // Skip bool values on properties that don't take IInspectable (struct/enum/string props)
                    std::string pk_lo_bool = pk;
                    for (auto& c : pk_lo_bool) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    bool skip_bool = (pk_lo_bool == "cornerradius" || pk_lo_bool == "margin"
                        || pk_lo_bool == "padding" || pk_lo_bool == "borderthickness"
                        || pk_lo_bool == "style" || pk_lo_bool == "fontfamily"
                        || pk_lo_bool == "fontsize" || pk_lo_bool == "itemcontainerstyle"
                        || pk_lo_bool == "itemtemplate" || pk_lo_bool == "itemssource"
                        || pk_lo_bool == "selectionmode" || pk_lo_bool == "data"
                        || pk_lo_bool == "fill" || pk_lo_bool == "stroke"
                        || pk_lo_bool == "background" || pk_lo_bool == "foreground"
                        || pk_lo_bool == "text" || pk_lo_bool == "content"
                        || pk_lo_bool == "header" || pk_lo_bool == "child"
                        || pk_lo_bool == "source" || pk_lo_bool == "glyph"
                        || pk_lo_bool == "placeholdertext" || pk_lo_bool == "tag"
                        || pk_lo_bool == "description" || pk_lo_bool == "minwidth"
                        || pk_lo_bool == "maxwidth" || pk_lo_bool == "minheight"
                        || pk_lo_bool == "maxheight" || pk_lo_bool == "width"
                        || pk_lo_bool == "height" || pk_lo_bool == "opacity"
                        || pk_lo_bool == "fontweight" || pk_lo_bool == "fontstyle"
                        || pk_lo_bool == "textwrapping" || pk_lo_bool == "texttrimming"
                        || pk_lo_bool == "textalignment" || pk_lo_bool == "lineheight"
                        || pk_lo_bool == "rowspacing" || pk_lo_bool == "columnspacing"
                        || pk_lo_bool == "rotation" || pk_lo_bool == "offsetx"
                        || pk_lo_bool == "offsety" || pk_lo_bool == "scalex"
                        || pk_lo_bool == "scaley" || pk_lo_bool == "resources"
                        || pk_lo_bool == "flyout" || pk_lo_bool == "tooltip"
                        || pk_lo_bool == "numberformatter" || pk_lo_bool == "maxlength"
                        || pk_lo_bool == "ismenuopen" || pk_lo_bool == "title"
                        || pk_lo_bool == "isexpanded" || pk_lo_bool == "ishitvisible"
                        || pk_lo_bool == "isreadonly" || pk_lo_bool == "isopen"
                        || pk_lo_bool == "isactive" || pk_lo_bool == "isindeterminate"
                        || pk_lo_bool == "value" || pk_lo_bool == "minimum"
                        || pk_lo_bool == "maximum" || pk_lo_bool == "stepfrequency"
                        || pk_lo_bool == "disabledforeground" || pk_lo_bool == "titleforeground"
                        || pk_lo_bool == "trackvalue" || pk_lo_bool == "snaptoticks"
                        || pk_lo_bool == "x1" || pk_lo_bool == "y1"
                        || pk_lo_bool == "x2" || pk_lo_bool == "y2"
                        || pk_lo_bool == "strokethickness" || pk_lo_bool == "radiusx"
                        || pk_lo_bool == "radiusy" || pk_lo_bool == "selectedindex"
                        || pk_lo_bool == "selecteditem" || pk_lo_bool == "selectedvalue"
                        || pk_lo_bool == "isdropdownopen" || pk_lo_bool == "itemslist"
                        || pk_lo_bool == "tabitems" || pk_lo_bool == "menuitems");
                    if (!skip_bool) {
                        if (pk_lo_bool == "ischecked")
                        {
                            if (is_plain_bool_ischecked(n.control))
                                out << indent << var_name << "." << prop << "(" << val << ");\n";
                            else
                                out << indent << var_name << "." << prop << "(winrt::Windows::Foundation::IReference<bool>(winrt::box_value(" << val << ")));\n";
                        }
                        else if (pk_lo_bool == "oncontent" || pk_lo_bool == "offcontent")
                            out << indent << var_name << "." << prop << "(winrt::box_value(" << val << "));\n";
                        else
                            out << indent << var_name << "." << prop << "(" << val << ");\n";
                    }
                } else {
                    if (prop == "SelectedIndex" || prop == "selectedindex")
                        out << indent << "try { " << var_name << "." << prop << "(" << val << "); } catch (...) {}\n";
                    else if (prop == "TargetInkCanvas") {
                        out << indent << "if (" << val << ".get_abi()) { " << var_name << "." << prop << "(" << val << "); }\n";
                        out << indent << "m_bindings.push_back([this, " << cap_var << "]() mutable { if (" << val << ".get_abi()) " << var_name << "." << prop << "(" << val << "); });\n";
                    } else
                        out << indent << var_name << "." << prop << "(" << val << ");\n";
                    if (val.find('?') != std::string::npos || val.find("L\"") != std::string::npos)
                        out << indent << "m_bindings.push_back([this, " << cap_var << "]() mutable { " << var_name << "." << prop << "(" << val << "); });\n";
                }
            }
            else if (!val.empty())
            {
                // Bool literals (unquoted): True/False -> true/false
                if (val == "True") val = "true";
                else if (val == "False") val = "false";
                // Enum literals (unquoted) — property-aware
                else if (val == "Collapsed" || val == "Visible")
                {
                    std::string pk_lo2 = pk;
                    for (auto& c : pk_lo2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (pk_lo2 == "visible" || pk_lo2 == "visibility")
                        val = "winrt::Microsoft::UI::Xaml::Visibility::" + val;
                    else if (pk_lo2 == "isbackbuttonvisible" || pk_lo2 == "previousbuttonvisibility" || pk_lo2 == "nextbuttonvisibility")
                        val = "winrt::Microsoft::UI::Xaml::Controls::NavigationViewBackButtonVisible::" + val;
                    else if (pk_lo2.find("scrollbarvisibility") != std::string::npos)
                        val = "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::" + val;
                    else
                        val = wstr_literal(val);
                }
                else if (val == "Auto" || val == "Disabled" || val == "Hidden")
                {
                    std::string pk_lo_auto = pk;
                    for (auto& c : pk_lo_auto) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (pk_lo_auto.find("scrollbarvisibility") != std::string::npos)
                        val = "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::" + val;
                    else if (pk_lo_auto.find("scrollmode") != std::string::npos)
                        val = "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::" + val;
                    else if (pk.find("paneDisplayMode") != std::string::npos || pk.find("displayMode") != std::string::npos)
                    {
                        if (val == "Auto") val = "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::Auto";
                        else if (val == "Disabled") val = "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Disabled";
                        else val = wstr_literal(val);
                    }
                    else
                        val = wstr_literal(val);
                }
                else if (val == "Left" || val == "Top" || val == "LeftCompact" || val == "LeftMinimal")
                {
                    if (pk.find("paneDisplayMode") != std::string::npos || pk.find("displayMode") != std::string::npos)
                        val = "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::" + val;
                    else
                    {
                        std::string resolved = resolve_prop_value(pk, val);
                        if (!resolved.empty()) val = resolved;
                        else val = wstr_literal(val);
                    }
                }
                else if (val == "Horizontal" || val == "Vertical")
                {
                    if (pk.find("orientation") != std::string::npos || pk.find("Orientation") != std::string::npos)
                        val = "winrt::Microsoft::UI::Xaml::Controls::Orientation::" + val;
                    else val = wstr_literal(val);
                }

                // Special: .Symbol("Settings") -> .Symbol(symbol_from_name(L"Settings"))
                bool handled_symbol = false;
                {
                    std::string pk_lo_sym = pk;
                    for (auto& c : pk_lo_sym) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (pk_lo_sym == "symbol")
                    {
                        std::string sym_name = val;
                        if (sym_name.size() > 3 && sym_name[0] == 'L' && sym_name[1] == '"')
                            sym_name = sym_name.substr(2, sym_name.size() - 3);
                        else if (sym_name.size() > 2 && sym_name.front() == '"' && sym_name.back() == '"')
                            sym_name = sym_name.substr(1, sym_name.size() - 2);
                        if (!sym_name.empty() && sym_name[0] != '{')
                            out << indent << var_name << "." << prop << "(symbol_from_name(L\"" << sym_name << "\"));\n";
                        else
                            out << indent << var_name << "." << prop << "(symbol_from_name(L\"None\"));\n";
                        handled_symbol = true;
                    }
                }
                if (!handled_symbol)
                // Emit bool/enum directly - do NOT fall through to L" wrapping
                if (val == "true" || val == "false" || val.find("::") != std::string::npos)
                {
                    // Bool values: emit raw true/false for most props, box_value for IInspectable props
                    std::string prop_lo_check = prop;
                    for (auto& ch : prop_lo_check) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    bool needs_bool_box = (prop_lo_check == "ischecked" || prop_lo_check == "oncontent" || prop_lo_check == "offcontent");
                    if (val == "true" || val == "false") {
                        if (prop_lo_check == "ischecked" && !is_plain_bool_ischecked(n.control))
                            out << indent << var_name << "." << prop << "(winrt::Windows::Foundation::IReference<bool>(winrt::box_value(" << val << ")));\n";
                        else if (prop_lo_check == "ischecked")
                            out << indent << var_name << "." << prop << "(" << val << ");\n";
                        else if (needs_bool_box)
                            out << indent << var_name << "." << prop << "(winrt::box_value(" << val << "));\n";
                        else
                            out << indent << var_name << "." << prop << "(" << val << ");\n";
                    } else
                        out << indent << var_name << "." << prop << "(" << val << ");\n";
                }
                // CSS color formats (hsl, oklch, display-p3)
                else if (!val.empty() && (val.substr(0,4) == "hsl(" || val.substr(0,5) == "hsla(" || val.substr(0,6) == "oklch(" || val.substr(0,17) == "color(display-p3,"))
                {
                    std::string css = try_css_color(val);
                    if (!css.empty())
                    {
                        bool is_brush = is_brush_property(pk);
                        val = is_brush
                            ? "make_solid_brush(" + css + ")"
                            : css;
                        out << indent << var_name << "." << prop << "(" << val << ");\n";
                    }
                    else
                        val = wstr_literal(val);
                }
                else if (val.size() >= 7 && val[0] == '#')
                {
                    // Hex color in unquoted prop value -> SolidColorBrush or ColorHelper
                    bool is_brush = is_brush_property(pk);
                    std::string prefix = is_brush
                        ? "make_solid_brush(winrt::Windows::UI::Color{"
                        : "winrt::Windows::UI::Color{";
                    std::string bsuffix = is_brush ? "})" : "})";
                    if (val.size() == 9)
                        val = prefix + hex_to_argb(val) + bsuffix;
                    else if (val.size() == 7)
                        val = prefix + "255, " + hex_to_rgb(val) + bsuffix;
                    else
                        val = wstr_literal(val);
                    out << indent << var_name << "." << prop << "(" << val << ");\n";
                }
                else
                {
                    // Check for binding expressions BEFORE wrapping in L"..."
                    bool is_binding = (val.find("self->") != std::string::npos)
                        || (val.find("x:Bind") != std::string::npos)
                        || (!val.empty() && val[0] == '$');

                    // Check for bare state variable references
                    if (!is_binding) {
                        for (auto const& sv : view.state_vars) {
                            if (val.size() >= sv.name.size()
                                && val.substr(0, sv.name.size()) == sv.name
                                && (val.size() == sv.name.size()
                                    || val[sv.name.size()] == '.'
                                    || val[sv.name.size()] == '('))
                            {
                                is_binding = true;
                                val = "self->" + val;
                                break;
                            }
                        }
                    }

                    if (is_binding) {
                        // Strip $ prefix from binding expressions like $slider3()
                        while (!val.empty() && val[0] == '$') val.erase(0, 1);
                        // Strip trailing () from simple binding references like slider3()
                        if (val.size() >= 2 && val.back() == '(' && val[val.size()-2] == ')')
                            val.erase(val.size() - 2);
                        // C# → C++ expression fixups for x:Bind bindings
                        // Remove self-> (member access, not pointer)
                        while (true) {
                            auto pos = val.find("self->");
                            if (pos == std::string::npos) break;
                            val.erase(pos, 6);
                        }
                        // .Value.Equals(x) → .Value() == x
                        while (true) {
                            auto pos = val.find(".Value.Equals(");
                            if (pos == std::string::npos) break;
                            val.replace(pos, 13, ".Value() == ");
                            // Remove trailing )
                            auto close = val.rfind(')');
                            if (close != std::string::npos) val.erase(close, 1);
                        }
                        // .Value.value → .Value().Value()
                        while (true) {
                            auto pos = val.find(".Value.value");
                            if (pos == std::string::npos) break;
                            val.replace(pos, 11, ".Value().Value()");
                        }
                        // .value → .Value()  (C# property accessor on the
                        // projected element; bare () drops the property and
                        // passes the control wrapper itself - C2783)
                        while (true) {
                            auto pos = val.find(".value");
                            if (pos == std::string::npos) break;
                            val.replace(pos, 6, ".Value()");
                        }
                        // .isChecked → .IsChecked()
                        while (true) {
                            auto pos = val.find(".isChecked");
                            if (pos == std::string::npos) break;
                            val.replace(pos, 10, "().IsChecked()");
                        }
                        // Bool literals: False/True → false/true
                        // Only standalone (not part of identifier)
                        {
                            auto p = val.find(".Equals(False)");
                            if (p != std::string::npos) val.replace(p, 14, " == false");
                        }
                        {
                            auto p = val.find(".Equals(True)");
                            if (p != std::string::npos) val.replace(p, 13, " == true");
                        }
                        // Standalone False/True → false/true (word boundary check)
                        for (auto const* kw : {"False", "True"}) {
                            std::string repl = (kw[0] == 'F') ? "false" : "true";
                            size_t pos = 0;
                            while ((pos = val.find(kw, pos)) != std::string::npos) {
                                bool left_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(val[pos-1]));
                                size_t end = pos + std::strlen(kw);
                                bool right_ok = (end >= val.size()) || !std::isalnum(static_cast<unsigned char>(val[end]));
                                if (left_ok && right_ok) {
                                    val.replace(pos, std::strlen(kw), repl);
                                    pos += repl.size();
                                } else {
                                    pos += std::strlen(kw);
                                }
                            }
                        }
                        // Fix double-call artifacts like errorRB.IsChecked()() → errorRB.IsChecked()
                        while (val.size() >= 4 && val.substr(val.size() - 4) == "()()")
                            val.erase(val.size() - 2);
                        // Binding values need type-aware handling: box for IInspectable/IReference
                        // setters, skip for typed setters the binding can't satisfy (brush,
                        // visibility, element-target refs, image sources, element-name refs).
                        {
                            std::string pk_lo_bind = pk;
                            for (auto& c : pk_lo_bind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            bool is_brush_prop_b = is_brush_property(pk);
                            // Detect the leading variable of a chain like name.Value()
                            bool skip_binding = (pk_lo_bind == "target" || pk_lo_bind == "child"
                                || is_brush_prop_b);
                            bool is_source_ctrl = (pk_lo_bind == "source" || pk_lo_bind == "urisource");
                            if (is_source_ctrl) skip_binding = true; // hstring state vars can't feed ImageSource/Uri
                            // Chains ending in .X() on non-numeric state vars can't be evaluated
                            std::string head = val;
                            {
                                auto dot = val.find('.');
                                if (dot != std::string::npos) head = val.substr(0, dot);
                                auto lp = val.find('(');
                                if (lp != std::string::npos && head.size() > lp) head = head.substr(0, lp);
                            }
                            for (auto const& sv : view.state_vars)
                            {
                                if (head != sv.name) continue;
                                // Inferred control types win over the raw JSON type: the var is
                                // a real control member, so .value stays an accessor and the
                                // emitted C++ keeps the .Value() call the setter can consume.
                                if (inferred_types.count(head)) break;
                                std::string lo_t(sv.type);
                                for (auto& c : lo_t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                bool numeric = (lo_t.find("double") != std::string::npos || lo_t.find("int") != std::string::npos
                                    || lo_t.find("float") != std::string::npos || lo_t.find("bool") != std::string::npos
                                    || lo_t == "number");
                                if (val == sv.name || val == sv.name + ".Value()" || val == sv.name + ".Value"
                                    || val == sv.name + ".value" || val == sv.name + ".Text" || val == sv.name + ".text")
                                {
                                    val = sv.name;
                                }
                                else if (val.find('.') != std::string::npos)
                                {
                                    skip_binding = true; // other chains on numerics are element refs
                                }
                                break;
                            }
                            if (skip_binding)
                            {
                                // Typed setters: skip — binding produces a non-convertible type
                            }
                            else if (pk_lo_bind == "ischecked" && !is_plain_bool_ischecked(n.control))
                            {
                                out << indent << var_name << "." << prop << "(winrt::Windows::Foundation::IReference<bool>(winrt::box_value(" << val << ")));\n";
                                out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(winrt::Windows::Foundation::IReference<bool>(winrt::box_value(" << val << "))); });\n";
                                out << indent << var_name << ".add_Checked([this](auto const&, auto const&) { " << val << " = true; update_bindings(); });\n";
                                out << indent << var_name << ".add_Unchecked([this](auto const&, auto const&) { " << val << " = false; update_bindings(); });\n";
                            }
                            else if (pk_lo_bind == "ischecked")
                            {
                                out << indent << var_name << "." << prop << "(" << val << ");\n";
                                out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(" << val << "); });\n";
                                out << indent << var_name << ".add_Checked([this](auto const&, auto const&) { " << val << " = true; update_bindings(); });\n";
                                out << indent << var_name << ".add_Unchecked([this](auto const&, auto const&) { " << val << " = false; update_bindings(); });\n";
                            }
                            else if (pk_lo_bind == "ison")
                            {
                                bool is_dark_bind = (val == "isDarkMode");
                                std::string init_val = is_dark_bind ? "is_current_dark_theme()" : val;
                                out << indent << var_name << "." << prop << "(" << init_val << ");\n";
                                out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(" << init_val << "); });\n";
                                if (is_dark_bind)
                                    out << indent << var_name << ".add_Toggled([this, " << var_name << "](auto const&, auto const&) { isDarkMode = " << var_name << ".IsOn(); apply_app_theme(isDarkMode); update_bindings(); });\n";
                                else
                                    out << indent << var_name << ".add_Toggled([this, " << var_name << "](auto const&, auto const&) { " << val << " = " << var_name << ".IsOn(); update_bindings(); });\n";
                            }
                            else if (pk_lo_bind == "value" && (n.control == "Slider" || n.control == "ProgressBar"))
                            {
                                out << indent << var_name << "." << prop << "(" << val << ");\n";
                                out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(" << val << "); });\n";
                                if (n.control == "Slider")
                                    out << indent << var_name << ".add_ValueChanged([this, " << var_name << "](auto const&, auto const&) { " << val << " = " << var_name << ".Value(); update_bindings(); });\n";
                            }
                            else if (pk_lo_bind == "text" && (n.control == "Input" || n.control == "TextBox"))
                            {
                                // The bound state var may itself be a control (a
                                // TextBox-to-TextBox two-way mirror): read through its
                                // Text for display and write back through its Text —
                                // assigning a control to hstring (or vice versa) is a
                                // compile error otherwise.
                                std::string bound_read = val;
                                std::string bound_write = val + " = " + var_name + ".Text()";
                                for (auto const& sv : view.state_vars)
                                {
                                    if (sv.name == val)
                                    {
                                        if (sv.type.find("TextBox") != std::string::npos || sv.type.find("Input") != std::string::npos)
                                        {
                                            bound_read = val + ".Text()";
                                            bound_write = val + ".Text(" + var_name + ".Text())";
                                        }
                                        break;
                                    }
                                }
                                out << indent << var_name << "." << prop << "(" << bound_read << ");\n";
                                out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(" << bound_read << "); });\n";
                                out << indent << var_name << ".add_TextChanged([this, " << var_name << "](auto const&, auto const&) { " << bound_write << "; update_bindings(); });\n";
                            }
                            else if (pk_lo_bind == "oncontent" || pk_lo_bind == "offcontent")
                                out << indent << var_name << "." << prop << "(winrt::box_value(" << val << "));\n";
                            else
                            {
                                // Deferred binding on a state var: apply the same
                                // property-aware conversions as the direct path —
                                // typed setters cannot take a raw hstring state var.
                                std::string pk_lo_bind2 = pk;
                                for (auto& c : pk_lo_bind2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                std::string bval = val;
                                if (pk_lo_bind2 == "borderthickness" || pk_lo_bind2 == "margin" || pk_lo_bind2 == "padding")
                                {
                                    // Numeric-expression values (marginSlider.Value(),
                                    // member double) expand into a uniform Thickness;
                                    // string state vars go through the name parser.
                                    bool numeric_expr = !val.empty() && (std::isdigit(static_cast<unsigned char>(val[0])) || val[0] == '-'
                                        || val.find(".Value()") != std::string::npos || val.find(".Value") != std::string::npos);
                                    bool state_string = false;
                                    for (auto const& sv : view.state_vars)
                                        if (sv.name == val) { state_string = (sv.type.find("string") != std::string::npos || sv.type.find("hstring") != std::string::npos); break; }
                                    if (numeric_expr && !state_string)
                                        bval = "winrt::Microsoft::UI::Xaml::Thickness{" + val + ", " + val + ", " + val + ", " + val + "}";
                                    else
                                        bval = "thickness_from_name(" + val + ")";
                                }
                                else if (pk_lo_bind2 == "cornerradius")
                                    bval = "corner_from_name(" + val + ")";
                                else if (pk_lo_bind2 == "style")
                                    bval = "style_from_name(" + val + ")";
                                else if (pk_lo_bind2 == "visible" || pk_lo_bind2 == "visibility")
                                    bval = "make_visibility(" + val + ")";
                                else if (pk_lo_bind2.find("alignment") != std::string::npos && pk_lo_bind2.find("horizontal") != std::string::npos)
                                    bval = "halignment_from_name(" + val + ")";
                                else if (pk_lo_bind2.find("alignment") != std::string::npos)
                                    bval = "valignment_from_name(" + val + ")";
                                if (is_numeric_prop(n.control, pk) && bval.find(".Value()") == std::string::npos)
                                {
                                    // Numeric DP fed a string state var: no C++ conversion exists.
                                    out << indent << "// " << var_name << "." << prop << " skipped: non-numeric state var on numeric property\n";
                                    continue;
                                }
                                else
                                {
                                    out << indent << var_name << "." << prop << "(" << bval << ");\n";
                                    out << indent << "m_bindings.push_back([this, " << cap_var << "]() mutable { " << var_name << "." << prop << "(" << bval << "); });\n";
                                }
                            }
                        }
                    } else {
                        // Special: icon property -> .Icon(SymbolIcon(Symbol::Xxx))
                        std::string icon_val = resolve_icon_value(pk, val);
                        if (!icon_val.empty())
                        {
                            std::string lo_pk_icon;
                            lo_pk_icon.reserve(pk.size());
                            for (auto c : pk) lo_pk_icon += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            std::string icon_prop = (lo_pk_icon == "iconsource") ? "IconSource" : "Icon";
                            // Projected types are standalone structs — no .as<>() or implicit conversion.
                            // Use scoped temp + from_abi for Icon (takes IconElement).
                            if (icon_prop == "Icon")
                                out << indent << "{ auto __ic = " << icon_val << "; " << var_name << ".Icon(IconElement::wrap_borrowed(__ic.m_unknown.get())); }\n";
                            else
                                out << indent << "{ auto __ic = " << icon_val << "; " << var_name << ".IconSource(IconSource::wrap_borrowed(__ic.m_unknown.get())); }\n";
                        }
                        // Normalize Image/ImageIcon Source paths
                        else if (pk == "source" && (n.control == "Image" || n.control == "ImageIcon" || n.control == "BitmapIcon"))
                        {
                            std::string src_val = val;
                            if (src_val.size() > 3 && src_val[0] == 'L' && src_val[1] == '"')
                                src_val = src_val.substr(2, src_val.size() - 3);
                            else if (src_val.size() >= 2 && src_val.front() == '"' && src_val.back() == '"')
                                src_val = src_val.substr(1, src_val.size() - 2);
                            for (auto& c : src_val) if (c == '\\') c = '/';
                            if (src_val.find("{ThemeResource ") != std::string::npos || src_val.find("{StaticResource ") != std::string::npos || (src_val.find('/') == std::string::npos && src_val.find('.') == std::string::npos))
                            {
                                std::string res_key = src_val;
                                auto p = res_key.find("Resource ");
                                if (p != std::string::npos) {
                                    res_key = res_key.substr(p + 9);
                                    if (!res_key.empty() && res_key.back() == '}') res_key.pop_back();
                                    while (!res_key.empty() && res_key.front() == ' ') res_key.erase(0, 1);
                                    while (!res_key.empty() && res_key.back() == ' ') res_key.pop_back();
                                }
                                out << indent << var_name << ".Source(image_source_lookup_th(L\"" << escape_wstr(res_key) << "\"));\n";
                            }
                            else
                            {
                                if (!src_val.empty() && src_val[0] == '/')
                                    src_val = "ms-appx:///" + src_val.substr(1);
                                else if (src_val.rfind("Assets/", 0) == 0)
                                    src_val = "ms-appx:///" + src_val;
                                out << indent << var_name << ".Source(winrt::Microsoft::UI::Xaml::Media::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << src_val << "\")));\n";
                            }
                        }
                        else if (!val.empty() && (std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-'))
                        {
                            // Numeric value - emit as bare number (e.g. MaxWidth(580))
                        out << indent << var_name << "." << prop << "(" << box_if_inspectable(n.control, pk, val) << ");\n";
                        }
                        else if (is_brush_property(pk))
                        {
                            if (is_known_color(val))
                            {
                                if (val == "Transparent") {
                                    out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));\n";
                                } else {
                                    out << indent << var_name << "." << prop << "(make_solid_brush(named_color(\"" << val << "\")));\n";
                                }
                            }
                            else
                            {
                                out << indent << var_name << "." << prop << "(theme_brush_th(L\"" << val << "\"));\n";
                            }
                        }
                        else
                        {
                            std::string collapsed;
                            for (char c : val) { if (c == '\n' || c == '\r') collapsed += ' '; else collapsed += c; }
                            std::string resolved = resolve_prop_value(pk, collapsed);
                            // Bare style name (no {StaticResource}): skip — the
                            // default theme already styles the control.
                            std::string pk_lo_st = pk;
                            for (auto& c : pk_lo_st) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (resolved.empty() && pk_lo_st == "style")
                            {
                                // no setter emitted
                            }
                            else if (!resolved.empty())
                            {
                                if (n.control == "TextBlock" && pk_lo_st == "style")
                                {
                                    if (resolved.find("DisplayTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(68);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (resolved.find("TitleLargeTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(40);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (resolved.find("TitleTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(28);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (resolved.find("SubtitleTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(20);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (resolved.find("BodyStrongTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(14);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (resolved.find("CaptionTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(12);\n";
                                    }
                                }
                                out << indent << var_name << "." << prop << "(" << resolved << ");\n";
                                if (resolved.find("AccentButtonStyle") != std::string::npos)
                                {
                                    out << indent << var_name << ".Background(theme_brush_th(L\"AccentFillColorDefaultBrush\"));\n";
                                    out << indent << var_name << ".Foreground(theme_brush_th(L\"TextOnAccentFillColorPrimaryBrush\"));\n";
                                }
                            }
                            else
                            {
                                // Try icon element construction (SymbolIcon, FontIcon, etc.)
                                std::string icon_val = resolve_icon_value(pk, collapsed);
                                if (!icon_val.empty())
                                {
                                    std::string lo_pk_i3 = pk;
                                    for (auto& c : lo_pk_i3) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (lo_pk_i3 == "icon" || lo_pk_i3 == "queryicon")
                                        out << indent << var_name << "." << prop << "(IconElement::wrap_borrowed((" << icon_val << ").get_abi()));\n";
                                    else if (lo_pk_i3 == "iconsource")
                                        out << indent << var_name << "." << prop << "(IconSource::wrap_borrowed((" << icon_val << ").get_abi()));\n";
                                    else
                                        out << indent << var_name << "." << prop << "(" << icon_val << ");\n";
                                }
                                else
                                {
                                    // Geometry-typed Data (PathIcon/Path): a raw string is
                                    // XAML path-data syntax — parse into a real PathGeometry
                                    // via the emitted helper.
                                    std::string lo_pk_geo3 = pk;
                                    for (auto& c : lo_pk_geo3) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (lo_pk_geo3 == "data" && collapsed.size() > 2
                                        && (collapsed.find('M') != std::string::npos || collapsed.find('m') != std::string::npos
                                            || collapsed.find('L') != std::string::npos || collapsed.find('C') != std::string::npos))
                                    {
                                        out << indent << var_name << "." << prop << "(parse_svg_path(\"" << collapsed << "\"));\n";
                                    }
                                    else
                                    {
                                        std::string interp = try_interpolate_string(collapsed, view.state_vars);
                                        if (!interp.empty())
                                        {
                                            // Struct/enum-typed setters need conversions a raw
                                            // interpolated string cannot provide.
                                            std::string pk_lo_sv3 = pk;
                                            for (auto& c : pk_lo_sv3) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                            bool skip_numeric_sv3 = is_numeric_prop(n.control, pk);
                                            if (pk_lo_sv3 == "borderthickness" || pk_lo_sv3 == "margin" || pk_lo_sv3 == "padding")
                                                interp = "thickness_from_name(" + interp + ")";
                                            else if (pk_lo_sv3 == "cornerradius")
                                                interp = "corner_from_name(" + interp + ")";
                                            else if (pk_lo_sv3 == "style")
                                                interp = "style_from_name(" + interp + ")";
                                            else if (pk_lo_sv3.find("alignment") != std::string::npos && pk_lo_sv3.find("horizontal") != std::string::npos)
                                                interp = "halignment_from_name(" + interp + ")";
                                            else if (pk_lo_sv3.find("alignment") != std::string::npos)
                                                interp = "valignment_from_name(" + interp + ")";
                                            if (skip_numeric_sv3)
                                            {
                                                // Numeric DP fed a string state var: no C++ conversion.
                                                out << indent << "// " << var_name << "." << prop << " skipped: non-numeric state var on numeric property\n";
                                            }
                                            else
                                            {
                                            std::string set_val = resolve_typed_value(n.control, pk, box_if_inspectable(n.control, pk, interp), &g_type_prop_kinds, &g_enum_info);
                                            out << indent << var_name << "." << prop << "(" << set_val << ");\n";
                                            out << indent << "m_bindings.push_back([this, " << cap_var << "]() mutable { " << var_name << "." << prop << "(" << set_val << "); });\n";
                                            }
                                        }
                                        else
                                        {
                                            std::string res_k = collapsed;
                                            bool is_res = false;
                                            if (res_k == "AppTitleName") {
                                                is_res = true;
                                            } else if (res_k.rfind("{StaticResource ", 0) == 0 || res_k.rfind("{ThemeResource ", 0) == 0) {
                                                auto sp = res_k.find(' ');
                                                res_k = res_k.substr(sp + 1);
                                                if (!res_k.empty() && res_k.back() == '}') res_k.pop_back();
                                                while (!res_k.empty() && res_k.front() == ' ') res_k.erase(0, 1);
                                                while (!res_k.empty() && res_k.back() == ' ') res_k.pop_back();
                                                is_res = true;
                                            }
                                            if (is_res)
                                            {
                                                std::string pk_lo_res = pk;
                                                for (auto& c : pk_lo_res) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                                if (is_brush_property(pk))
                                                    val = "theme_brush_th(L\"" + escape_wstr(res_k) + "\")";
                                                else if (pk_lo_res == "text")
                                                    val = "winrt::unbox_value_or<winrt::hstring>(theme_lookup_th(L\"" + escape_wstr(res_k) + "\", winrt::box_value(winrt::hstring(L\"" + escape_wstr(res_k) + "\"))), L\"" + escape_wstr(res_k) + "\")";
                                                else if (pk_lo_res == "data")
                                                    val = "geometry_lookup_th(L\"" + escape_wstr(res_k) + "\")";
                                                else if (pk_lo_res == "source" || pk_lo_res == "imagesource" || pk_lo_res == "iconsource")
                                                    val = "image_source_lookup_th(L\"" + escape_wstr(res_k) + "\")";
                                                else
                                                    val = "theme_lookup_th(L\"" + escape_wstr(res_k) + "\", winrt::box_value(winrt::hstring(L\"" + escape_wstr(res_k) + "\")))";
                                            }
                                            else
                                                val = wstr_literal(collapsed);
                                            out << indent << var_name << "." << prop << "(" << val << ");\n";
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            else { val = val; } // empty val, skip
        }

// Emit event handler subscriptions (skip raw(s,e) no-ops, deduplicate)
        std::set<std::string> emitted_events;
        for (auto const& ev : n.events)
        {
            std::string cb_name;
            if (!ev.actions.empty())
            {
                cb_name = ev.actions[0];
                auto paren = cb_name.find('(');
                if (paren != std::string::npos) cb_name = cb_name.substr(0, paren);
                while (!cb_name.empty() && cb_name.back() == '_') cb_name.pop_back();
            }
            else
            {
                std::string evname = ev.name;
                if (evname.size() > 2 && evname[0] == 'o' && evname[1] == 'n')
                    evname = evname.substr(2);
                if (!evname.empty()) evname[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(evname[0])));
                cb_name = "On" + evname;
            }
            if (cb_name == "raw") continue;
            std::string ev_key = var_name + "." + to_pascal(ev.name);
            if (emitted_events.count(ev_key)) continue;
            emitted_events.insert(ev_key);
            // Emit event subscription wrapped in lambda for delegate compatibility
            // RoutedEventHandler(L&& lambda) accepts any callable via template ctor
            {
                // Strip 'on' prefix for WinUI3 C++ event projection (onClick->Click, onLoaded->Loaded)
                std::string ev_prop_name = ev.name;
                if (ev_prop_name.size() > 2 && ev_prop_name[0] == 'o' && ev_prop_name[1] == 'n') {
                    ev_prop_name = ev_prop_name.substr(2);
                    if (!ev_prop_name.empty()) ev_prop_name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ev_prop_name[0])));
                }
                std::string evt_prop = ctrl_prop(n.control.c_str(), ev_prop_name);
                std::string handler_call = cb_name;
                if (handler_call.starts_with("self.")) handler_call = handler_call.substr(5);
                if (g_view_type_names.count(n.control) && n.control != class_name)
                {
                    out << indent << var_name << ".add_" << ev_prop_name
                        << "([this](auto const& s, auto const& e) { " << (handler_call.empty() ? "" : (handler_call + "(s, e); ")) << "});\n";
                }
                else
                    out << indent << var_name << ".add_" << evt_prop << "([this](auto const& s, auto const& e) { " << (handler_call.empty() ? "" : (handler_call + "(s, e); ")) << "});\n";
            }
        }
        // Emit action closures (skip raw(s,e) no-ops, deduplicate)
        for (auto const& act : n.action)
        {
            if (act.empty() || act[0] == '/' ) continue;
            if (act == "raw") continue;
            // Fix action value quotes: ensure L"" for wstring state vars, strip quotes for numeric vars
            std::string cleaned_act = act;
            {
                auto eq_pos = cleaned_act.find("= \"");
                if (eq_pos != std::string::npos) {
                    auto val_start = eq_pos + 2; // index of opening "
                    auto val_end = cleaned_act.rfind('"');
                    if (val_end > val_start) {
                        std::string inner_val = cleaned_act.substr(val_start + 1, val_end - val_start - 1);
                        // Determine if state var is string or numeric
                        std::string lhs = cleaned_act.substr(0, eq_pos);
                        while (!lhs.empty() && lhs.back() == ' ') lhs.pop_back();
                        bool is_string_var = false;
                        for (auto const& sv : view.state_vars) {
                            if (sv.name == lhs && (sv.type == "string" || to_cxx_type(sv.type) == "std::wstring")) {
                                is_string_var = true;
                                break;
                            }
                        }
                        if (is_string_var) {
                            // String var: ensure L"" prefix
                            cleaned_act = cleaned_act.substr(0, val_start) + "L\"" + inner_val + "\"" + cleaned_act.substr(val_end + 1);
                        } else {
                            // Numeric/bool var: strip quotes entirely
                            cleaned_act = cleaned_act.substr(0, val_start) + inner_val + cleaned_act.substr(val_end + 1);
                        }
                    }
                }
            }
            // Strip self-> from action expressions (member access in lambda)
            {
                size_t pos;
                while ((pos = cleaned_act.find("self->")) != std::string::npos)
                    cleaned_act.erase(pos, 6);
            }
            std::string act_key = var_name + ".Click_" + cleaned_act;
            if (emitted_events.count(act_key)) continue;
            emitted_events.insert(act_key);
            // Resolve binding expressions in action text
            std::string resolved_act = resolve_binding_to_cpp(cleaned_act, view.state_vars);
            // Check if action is an assignment (contains = but not == or !=)
            bool is_assignment = false;
            for (size_t i = 0; i < resolved_act.size(); ++i) {
                if (resolved_act[i] == '=' && (i == 0 || (resolved_act[i-1] != '!' && resolved_act[i-1] != '=' && resolved_act[i-1] != '<' && resolved_act[i-1] != '>')) && (i+1 >= resolved_act.size() || resolved_act[i+1] != '=')) {
                    is_assignment = true;
                    break;
                }
            }
            if (is_assignment) {
                // Ensure wide strings for wstring state vars
                std::string assign_rhs = resolved_act.substr(resolved_act.find('=') + 1);
                // Trim leading space
                while (!assign_rhs.empty() && assign_rhs[0] == ' ') assign_rhs.erase(0, 1);
                // Wrap narrow string literals in L"" for wstring state vars
                if (assign_rhs.size() >= 2 && assign_rhs.front() == '"' && assign_rhs.back() == '"') {
                    // Check if the state var is wstring
                    std::string lhs = resolved_act.substr(0, resolved_act.find('='));
                    while (!lhs.empty() && lhs.back() == ' ') lhs.pop_back();
                    for (auto const& sv : view.state_vars) {
                        if (sv.name == lhs && (sv.type == "string" || to_cxx_type(sv.type) == "std::wstring")) {
                            assign_rhs = "L" + assign_rhs;
                            break;
                        }
                    }
                }
                std::string act_lhs = resolved_act.substr(0, resolved_act.find('='));
                while (!act_lhs.empty() && act_lhs.back() == ' ') act_lhs.pop_back();
                while (!act_lhs.empty() && act_lhs.front() == ' ') act_lhs.erase(0, 1);
                std::string extra_call;
                if (act_lhs.find("isDarkMode") != std::string::npos)
                    extra_call = " apply_app_theme(isDarkMode);";
                out << indent << var_name << ".add_Click([this](auto const& s, auto const& e) { " << resolved_act.substr(0, resolved_act.find('=') + 1) << " " << assign_rhs << ";" << extra_call << " update_bindings(); });\n";

                // Auto-style navigation buttons (e.g. selectedNav = 0) with modern segmented pill active indicator
                if (act_lhs.find("Nav") != std::string::npos || act_lhs.find("nav") != std::string::npos || act_lhs.find("selected") != std::string::npos || act_lhs.find("Selected") != std::string::npos) {
                    std::string clean_rhs = assign_rhs;
                    while (!clean_rhs.empty() && (clean_rhs.back() == ';' || clean_rhs.back() == ' ')) clean_rhs.pop_back();
                    while (!clean_rhs.empty() && clean_rhs.front() == ' ') clean_rhs.erase(0, 1);
                    out << indent << var_name << ".CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{ 6, 6, 6, 6 });\n";
                    out << indent << var_name << ".Padding(winrt::Microsoft::UI::Xaml::Thickness{ 16, 6, 16, 6 });\n";
                    out << indent << var_name << ".BorderThickness(winrt::Microsoft::UI::Xaml::Thickness{ 0, 0, 0, 0 });\n";
                    out << indent << "auto update_nav_" << var_name << " = [this, " << var_name << "]() mutable {\n";
                    out << indent << "    bool is_active = (" << act_lhs << " == " << clean_rhs << ");\n";
                    out << indent << "    if (is_active) {\n";
                    out << indent << "        " << var_name << ".Background(theme_brush_th(L\"AccentFillColorDefaultBrush\"));\n";
                    out << indent << "        " << var_name << ".Foreground(theme_brush_th(L\"TextOnAccentFillColorPrimaryBrush\"));\n";
                    out << indent << "    } else {\n";
                    out << indent << "        " << var_name << ".Background(winrt::Microsoft::UI::Xaml::Media::Brush(make_solid_brush(winrt::Windows::UI::Color{0, 0, 0, 0})));\n";
                    out << indent << "        " << var_name << ".Foreground(theme_brush_th(L\"TextFillColorPrimaryBrush\"));\n";
                    out << indent << "    }\n";
                    out << indent << "    " << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{ static_cast<uint16_t>(is_active ? 600 : 400) });\n";
                    out << indent << "};\n";
                    out << indent << var_name << ".add_PointerEntered([this, " << var_name << "](auto const&, auto const&) {\n";
                    out << indent << "    bool is_active = (" << act_lhs << " == " << clean_rhs << ");\n";
                    out << indent << "    if (!is_active) {\n";
                    out << indent << "        bool is_lt = is_current_light_theme();\n";
                    out << indent << "        " << var_name << ".Background(winrt::Microsoft::UI::Xaml::Media::Brush(make_solid_brush(is_lt ? winrt::Windows::UI::Color{24, 0, 0, 0} : winrt::Windows::UI::Color{24, 255, 255, 255})));\n";
                    out << indent << "    } else {\n";
                    out << indent << "        " << var_name << ".Background(theme_brush_th(L\"AccentFillColorSecondaryBrush\"));\n";
                    out << indent << "    }\n";
                    out << indent << "});\n";
                    out << indent << var_name << ".add_PointerExited([this, " << var_name << "](auto const&, auto const&) {\n";
                    out << indent << "    bool is_active = (" << act_lhs << " == " << clean_rhs << ");\n";
                    out << indent << "    if (!is_active) {\n";
                    out << indent << "        " << var_name << ".Background(winrt::Microsoft::UI::Xaml::Media::Brush(make_solid_brush(winrt::Windows::UI::Color{0, 0, 0, 0})));\n";
                    out << indent << "    } else {\n";
                    out << indent << "        " << var_name << ".Background(theme_brush_th(L\"AccentFillColorDefaultBrush\"));\n";
                    out << indent << "    }\n";
                    out << indent << "});\n";
                    out << indent << "update_nav_" << var_name << "();\n";
                    out << indent << "m_bindings.push_back(update_nav_" << var_name << ");\n";
                }
            } else {
                // Method call: ensure it has parens
                if (resolved_act.find('(') == std::string::npos)
                    out << indent << var_name << ".add_Click([this](auto const& s, auto const& e) { " << resolved_act << "(); update_bindings(); });\n";
                else
                    out << indent << var_name << ".add_Click([this](auto const& s, auto const& e) { " << resolved_act << "; update_bindings(); });\n";
            }
        }
        // Control-aware property name for raw attrs: use ctrl_prop for plain props, to_pascal for attached
        auto raw_prop_name = [&](const std::string& rk_raw) -> std::string {
            if (rk_raw.find('.') != std::string::npos) return to_pascal(rk_raw);
            return ctrl_prop(n.control.c_str(), rk_raw);
        };
// Template payloads: factory function + ChoosingItemContainer (no load_xaml)
        for (auto const& [rk, rv] : n.raw_attrs)
        {
            if (is_template_payload(rk, rv))
            {
                template_factory_result tfr = template_payload_to_cpp_factory(rv, tpl_counter++, catalog);
                pending_factory_funcs.push_back(tfr);
                // ChoosingItemContainer event handler skipped - delegate type not projected
                (void)var_name;
                (void)tfr;
                continue;
            }
        }

        // Emit raw XAML attributes — handle colons (x:Name → SetProperty("x:Name",...))
        for (auto const& [rk, rv] : n.raw_attrs)
        {
            // Template JSON payloads are consumed by the template-factory pass
            // above — emitting them again as property strings is invalid (the
            // setter takes DataTemplate, and the JSON corrupts the literal).
            if (is_template_payload(rk, rv)) continue;
            // {TemplateBinding X} — a lowered ControlTemplate: the view class
            // IS the templated control, so read the HOST's own property X
            // (kept reactive through m_bindings) instead of boxing the markup
            // as a literal (CornerRadius{TemplateBinding ...} never compiled).
            // TB targets that match a host state var are skipped: the state
            // var's sanitized type (string) can't be proven against the
            // element's setter, and a mismatch breaks the module.
            if (rv.rfind("{TemplateBinding ", 0) == 0) {
                std::string tb = rv.substr(17);
                if (!tb.empty() && tb.back() == '}') tb.pop_back();
                while (!tb.empty() && (tb.front() == ' ' || tb.front() == '\t')) tb.erase(tb.begin());
                while (!tb.empty() && (tb.back() == ' ' || tb.back() == '\t')) tb.pop_back();
                std::string tb_lo = tb;
                for (auto& ch : tb_lo) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                bool host_state = false;
                for (auto const& sv : view.state_vars) {
                    std::string nlo = sv.name;
                    for (auto& ch : nlo) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (nlo == tb_lo) { host_state = true; break; }
                }
                if (!tb.empty() && !host_state) {
                    std::string tb_setter = raw_prop_name(rk);
                    out << indent << var_name << "." << tb_setter << "(this->" << tb << "());\n";
                    out << indent << "m_bindings.push_back([this, __tbv = " << var_name << "]() mutable { __tbv."
                        << tb_setter << "(this->" << tb << "()); });\n";
                }
                continue;
            }
            // Resource definitions (<DataTemplate x:Key=...>) misattached to a
            // node by the converter are NOT property values. Emitting them as
            // setters produced bogus calls like
            //   StackLayout.FlowItemTemplate(data_template_th(L"<brush key>"))
            // where the "key" was the first ThemeResource *inside* the template
            // XML. Real registration happens in the resources modules.
            {
                std::string dv = rv;
                if (!dv.empty() && dv.front() == '"') dv = dv.substr(1);
                std::string dl = dv;
                for (auto& c : dl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (dl.rfind("<datatemplate", 0) == 0) continue;
            }
            // Skip ALL raw attrs on component types (except registered app views)
            if (is_app_or_unknown_type(n.control) && !g_view_type_names.count(n.control) && n.control != "SettingsCard" && n.control != "SettingsExpander") continue;
            // TeachingTip.Target takes a FrameworkElement (x:Name ref) — drop plain-string attrs
            {
                std::string lo_ctrl_tt = n.control;
                for (auto& c : lo_ctrl_tt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string lo_rk_tt = rk;
                for (auto& c : lo_rk_tt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lo_ctrl_tt == "teachingtip" && lo_rk_tt == "target") continue;
            }
            // Skip collection/dictionary properties that cannot be set via simple value
            {
                std::string rk_lo_skip = rk;
                for (auto& c : rk_lo_skip) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (rk_lo_skip == "rowdefinitions" || rk_lo_skip == "columndefinitions" || rk_lo_skip == "grid.rowdefinitions" || rk_lo_skip == "grid.columndefinitions"
                    || rk_lo_skip == "children" || rk_lo_skip == "inlines"
                    || rk_lo_skip == "menuitems" || rk_lo_skip == "tabitems"
                    || rk_lo_skip == "primarycommands" || rk_lo_skip == "secondarycommands"
                    || rk_lo_skip == "flyout" || rk_lo_skip == "styles"
                    || rk_lo_skip == "templates"                    || rk_lo_skip == "exittransition" || rk_lo_skip == "opentransition" || rk_lo_skip == "ticks"
                    || rk_lo_skip == "backgroundtransition"
                    || rk_lo_skip == "linebreak" || rk_lo_skip == "containercontentchanging"
                    || rk_lo_skip == "choosingitemcontainer" || rk_lo_skip == "choosingsemanticzoomstate"
                    || rk_lo_skip == "container" || rk_lo_skip == "overrideuicommand"
                    || rk_lo_skip == "opacitymask"
                    || rk_lo_skip == "prefix" || rk_lo_skip == "substitutions" || rk_lo_skip == "columns" || rk_lo_skip == "groupstyle" || (n.control == "ItemsRepeater" && rk_lo_skip == "datatemplate")) continue;
            }
            std::string rval = rv;
            // Collapse embedded newlines in raw attr values
            {
                std::string clean;
                clean.reserve(rval.size());
                for (char c : rval) { if (c == '\n' || c == '\r') clean += ' '; else clean += c; }
                rval = clean;
            }
            // Extract inner value from quoted strings first, then resolve enums
            {
                std::string inner = rval;
                bool was_quoted = (rval.size() >= 2 && rval.front() == '"' && rval.back() == '"');
                if (was_quoted)
                    inner = rval.substr(1, rval.size() - 2);
                // Collapse newlines
                std::string collapsed;
                for (char c : inner) { if (c == '\n' || c == '\r') collapsed += ' '; else collapsed += c; }
                // Property-aware key name (lowercase)
                std::string rk_lower = rk;
                for (auto& c : rk_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                bool resolved = false;
                if (collapsed == "True" || collapsed == "true") { rval = "true"; resolved = true; }
                else if (collapsed == "False" || collapsed == "false") { rval = "false"; resolved = true; }
                else if (collapsed == "Collapsed" || collapsed == "Visible")
                {
                    if (rk_lower == "visibility" || rk_lower == "visible" || rk_lower.find("alignment") != std::string::npos)
                        rval = "winrt::Microsoft::UI::Xaml::Visibility::" + collapsed;
                    else if (rk_lower.find("scrollbarvisibility") != std::string::npos)
                        rval = "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::" + collapsed;
                    else if (rk_lower.find("backbuttonvisible") != std::string::npos)
                        rval = "winrt::Microsoft::UI::Xaml::Controls::NavigationViewBackButtonVisible::" + collapsed;
                    else
                        rval = "winrt::Microsoft::UI::Xaml::Visibility::" + collapsed;
                    resolved = true;
                }
                else if (collapsed == "Auto" || collapsed == "Disabled" || collapsed == "Hidden")
                {
                    if (rk_lower.find("scrollbarvisibility") != std::string::npos)
                        rval = "winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::" + collapsed;
                    else if (rk_lower.find("scrollmode") != std::string::npos)
                        rval = "winrt::Microsoft::UI::Xaml::Controls::ScrollMode::" + collapsed;
                    else if (rk_lower.find("displaymode") != std::string::npos)
                        rval = "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::" + collapsed;
                    else
                        rval = wstr_literal(collapsed);
                    resolved = true;
                }
                else if (collapsed == "Left" || collapsed == "Top" || collapsed == "LeftCompact" || collapsed == "LeftMinimal")
                {
                    if (rk_lower.find("displaymode") != std::string::npos)
                        rval = "winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode::" + collapsed;
                    else
                    {
                        std::string r = resolve_prop_value(rk, collapsed);
                        if (!r.empty()) rval = r; else rval = wstr_literal(collapsed);
                    }
                    resolved = true;
                }
                else if (collapsed == "Horizontal" || collapsed == "Vertical")
                {
                    if (rk_lower.find("orientation") != std::string::npos)
                        rval = "winrt::Microsoft::UI::Xaml::Controls::Orientation::" + collapsed;
                    else
                        rval = wstr_literal(collapsed);
                    resolved = true;
                }
                else if (collapsed == "Informational" || collapsed == "Success" || collapsed == "Warning" || collapsed == "Error")
                {
                    std::string r = resolve_prop_value(rk, collapsed);
                    if (!r.empty()) rval = r; else rval = wstr_literal(collapsed);
                    resolved = true;
                }
                else if (collapsed == "Wrap" || collapsed == "NoWrap" || collapsed == "WrapWholeWords")
                {
                    std::string r = resolve_prop_value(rk, collapsed);
                    if (!r.empty()) rval = r; else rval = wstr_literal(collapsed);
                    resolved = true;
                }
                else if (collapsed == "Italic" || collapsed == "Oblique")
                {
                    std::string r = resolve_prop_value(rk, collapsed);
                    if (!r.empty()) rval = r; else rval = wstr_literal(collapsed);
                    resolved = true;
                }
                else if (collapsed == "Single" || collapsed == "Multiple" || collapsed == "Extended")
                {
                    std::string r = resolve_prop_value(rk, collapsed);
                    if (!r.empty()) rval = r; else rval = wstr_literal(collapsed);
                    resolved = true;
                }
                else if (collapsed == "Compact" || collapsed == "Inline")
                {
                    std::string r = resolve_prop_value(rk, collapsed);
                    if (!r.empty()) rval = r; else rval = wstr_literal(collapsed);
                    resolved = true;
                }
                else if (!collapsed.empty() && collapsed != "nullptr" && !std::isdigit(static_cast<unsigned char>(collapsed.front())))
                {
                    std::string r = resolve_prop_value(rk, collapsed);
                    if (!r.empty()) { rval = r; resolved = true; }
                }
                if (!resolved)
                {
                    if (!collapsed.empty() && (std::isdigit(static_cast<unsigned char>(collapsed.front())) || collapsed.front() == '-'))
                    {
                        std::string rk_lo = rk_lower;
                        if (rk_lo == "padding" || rk_lo == "margin" || rk_lo == "borderthickness" || rk_lo == "cornerradius"
                            || rk_lo == "background" || rk_lo == "foreground" || rk_lo == "fill" || rk_lo == "stroke"
                            || rk_lo == "selectionhighlightcolor" || rk_lo == "visibility"
                            || rk_lo == "flowdirection" || rk_lo == "requestedtheme" || rk_lo == "displaymode"
                            || rk_lo == "placement" || rk_lo == "preferredplacement")
                        {
                            std::string resolved = resolve_prop_value(rk, collapsed);
                            if (!resolved.empty()) rval = resolved;
                            else rval = collapsed;
                        }
                        else
                            rval = collapsed;
                    }
                    else if (was_quoted || (!collapsed.empty() && (collapsed.front() == '{' || collapsed.front() == '<' || collapsed.front() == '[')))
                    {
                        std::string interp = try_interpolate_string(collapsed, view.state_vars);
                        if (!interp.empty())
                        {
                            rval = interp;
                            resolved = true;
                        }
                        else
                            rval = wstr_literal(collapsed);
                    }
                    bool is_xml_fragment = (!collapsed.empty() && collapsed.front() == '<') ||
                        (!rval.empty() && rval.front() == '<') ||
                        (rval.size() > 2 && rval[0] == 'L' && rval[1] == '"' && rval[2] == '<');
                    // {ThemeResource Key} -> guarded resource lookup with a
                    // codegen-baked literal fallback. Handles both the raw
                    // markup form (special keys kept rval = collapsed) and the
                    // quoted form (rval = L"{ThemeResource ...}").
                    if (!is_xml_fragment && rval.find("ThemeResource ") != std::string::npos)
                    {
                        std::string tr = rval;
                        if (tr.size() > 2 && tr[0] == 'L' && tr[1] == '"') tr = tr.substr(2, tr.size() - 3);
                        else if (tr.size() >= 2 && tr.front() == '"' && tr.back() == '"') tr = tr.substr(1, tr.size() - 2);
                        auto ks = tr.find("ThemeResource ") + 14;
                        auto ke = tr.find('}', ks);
                        if (ke != std::string::npos)
                        {
                            std::string res_key = tr.substr(ks, ke - ks);
                            std::string rk_lo_tr = rk;
                            for (auto& c : rk_lo_tr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            std::string kind;
                            std::string fallback;
                            if (rk_lo_tr == "background" || rk_lo_tr == "foreground"
                                || rk_lo_tr == "fill" || rk_lo_tr == "stroke"
                                || rk_lo_tr == "borderbrush"
                                || rk_lo_tr == "rowbackground" || rk_lo_tr == "alternatingrowbackground"
                                || rk_lo_tr == "axislinebrush" || rk_lo_tr == "tickbrush"
                                || rk_lo_tr == "gridlinemajorbrush" || rk_lo_tr == "gridlineminorbrush"
                                || rk_lo_tr == "datamarkerbrush" || rk_lo_tr == "databrush"
                                || rk_lo_tr == "markerbrush" || rk_lo_tr == "legendbrush")
                            {
                                kind = "brush";
                                fallback = "winrt::IInspectable{nullptr}";
                            }
                            else if (rk_lo_tr == "padding" || rk_lo_tr == "margin"
                                || rk_lo_tr == "borderthickness")
                            {
                                kind = "thickness";
                                if (res_key == "PagePadding") fallback = "winrt::Microsoft::UI::Xaml::Thickness{40, 24, 40, 40}";
                                else if (res_key == "CardPadding") fallback = "winrt::Microsoft::UI::Xaml::Thickness{20, 20, 20, 20}";
                                else if (res_key == "InnerCardPadding") fallback = "winrt::Microsoft::UI::Xaml::Thickness{16, 16, 16, 16}";
                                else fallback = "winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 0}";
                            }
                            else if (rk_lo_tr == "spacing" || rk_lo_tr == "linespacing"
                                || rk_lo_tr == "maxheight" || rk_lo_tr == "minheight"
                                || rk_lo_tr == "maxwidth" || rk_lo_tr == "minwidth"
                                || rk_lo_tr == "fontsize")
                            {
                                kind = "double";
                                if (res_key == "LayoutSpacing") fallback = "24.0";
                                else if (res_key == "ElementSpacing") fallback = "16.0";
                                else if (res_key == "CardSpacing") fallback = "12.0";
                                else fallback = "0.0";
                            }
                            else
                            {
                                kind = "other";
                                fallback = "winrt::IInspectable{nullptr}";
                            }
                            if (rk_lo_tr == "fontfamily")
                            {
                                // FontFamily resources resolve to the default UI
                                // font; the projected class only takes the name.
                                rval = "winrt::Microsoft::UI::Xaml::Media::FontFamily(L\"Segoe UI\")";
                            }
                            else if (kind == "brush")
                                rval = "theme_brush_th(L\"" + res_key + "\")";
                            else if (kind == "thickness")
                                rval = fallback;
                            else if (rk_lo_tr == "template")
                                rval = "control_template_th(L\"" + res_key + "\")";
                            else if (rk_lo_tr == "itemtemplate" || rk_lo_tr == "contenttemplate"
                                || rk_lo_tr == "headertemplate" || rk_lo_tr == "footertemplate"
                                || rk_lo_tr.ends_with("template"))
                                rval = "data_template_th(L\"" + res_key + "\")";
                            else if (kind == "double")
                                rval = "winrt::unbox_value_or<double>(theme_lookup_th(L\"" + res_key + "\", winrt::box_value(" + fallback + ")), " + fallback + ")";
                            else
                                rval = "theme_lookup_th(L\"" + res_key + "\", " + fallback + ")";
                        }
                    }
                    // {StaticResource Key} -> same resolution order as ThemeResource
                    if (!is_xml_fragment && rval.find("StaticResource ") != std::string::npos)
                    {
                        std::string tr = rval;
                        if (tr.size() > 2 && tr[0] == 'L' && tr[1] == '"') tr = tr.substr(2, tr.size() - 3);
                        else if (tr.size() >= 2 && tr.front() == '"' && tr.back() == '"') tr = tr.substr(1, tr.size() - 2);
                        auto ks = tr.find("StaticResource ") + 15;
                        auto ke = tr.find('}', ks);
                        if (ke != std::string::npos)
                        {
                            std::string res_key = tr.substr(ks, ke - ks);
                            std::string rk_lo_sr = rk;
                            for (auto& c : rk_lo_sr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (rk_lo_sr == "style")
                                rval = "style_lookup_th(L\"" + res_key + "\")";
                            else if (rk_lo_sr == "template")
                                rval = "control_template_th(L\"" + res_key + "\")";
                            else if (rk_lo_sr == "itemtemplate" || rk_lo_sr == "contenttemplate"
                                || rk_lo_sr == "headertemplate" || rk_lo_sr == "footertemplate"
                                || rk_lo_sr.ends_with("template"))
                                rval = "data_template_th(L\"" + res_key + "\")";
                            else if (rk_lo_sr == "background" || rk_lo_sr == "foreground"
                                || rk_lo_sr == "fill" || rk_lo_sr == "stroke"
                                || rk_lo_sr == "borderbrush"
                                || rk_lo_sr == "rowbackground" || rk_lo_sr == "alternatingrowbackground"
                                || rk_lo_sr == "axislinebrush" || rk_lo_sr == "tickbrush"
                                || rk_lo_sr == "gridlinemajorbrush" || rk_lo_sr == "gridlineminorbrush"
                                || rk_lo_sr == "datamarkerbrush" || rk_lo_sr == "databrush"
                                || rk_lo_sr == "markerbrush" || rk_lo_sr == "legendbrush")
                                rval = "theme_brush_th(L\"" + res_key + "\")";
                            else if (rk_lo_sr == "padding" || rk_lo_sr == "margin"
                                || rk_lo_sr == "borderthickness")
                            {
                                if (res_key == "PagePadding") rval = "winrt::Microsoft::UI::Xaml::Thickness{40, 24, 40, 40}";
                                else if (res_key == "CardPadding") rval = "winrt::Microsoft::UI::Xaml::Thickness{20, 20, 20, 20}";
                                else if (res_key == "InnerCardPadding") rval = "winrt::Microsoft::UI::Xaml::Thickness{16, 16, 16, 16}";
                                else rval = "winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 0}";
                            }
                            else if (rk_lo_sr == "spacing" || rk_lo_sr == "linespacing"
                                || rk_lo_sr == "rowspacing" || rk_lo_sr == "columnspacing"
                                || rk_lo_sr == "maxheight" || rk_lo_sr == "minheight"
                                || rk_lo_sr == "maxwidth" || rk_lo_sr == "minwidth"
                                || rk_lo_sr == "fontsize")
                            {
                                std::string fb = "0.0";
                                if (res_key == "LayoutSpacing") fb = "24.0";
                                else if (res_key == "ElementSpacing") fb = "16.0";
                                else if (res_key == "CardSpacing") fb = "12.0";
                                rval = "winrt::unbox_value_or<double>(theme_lookup_th(L\"" + res_key + "\", winrt::box_value(" + fb + ")), " + fb + ")";
                            }
                            else
                                rval = "theme_lookup_th(L\"" + res_key + "\", winrt::IInspectable{nullptr})";
                        }
                    }
                }
            }
            // IsSelected on ComboBoxItem (bool property, not an event - the
            // old suffix heuristic misrouted it into a commented-out
            // add_IsSelected stub so no item ever showed as selected) and
            // IsOn on ToggleSwitch must be applied before event detection.
            {
                std::string rk_lo_sel = rk;
                for (auto& c : rk_lo_sel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if ((rk_lo_sel == "isselected" || rk_lo_sel == "ison")
                    && (rval == "true" || rval == "false"))
                {
                    std::string prop = (rk_lo_sel == "isselected") ? "IsSelected" : "IsOn";
                    out << indent << var_name << "." << prop << "(" << rval << ");\n";
                    continue;
                }
            }
            // Detect raw attrs that are event handler registrations
            // Pattern: key like "DisplayModeChanged", "PaneToggleRequested", "Invoked", "Navigated"
            //          value like "OnPaneDisplayModeChanged", "TitleBar_PaneToggleRequested"
            {
                bool looks_like_event = false;
                std::string evt_handler = rv; // use original unescaped value
                // Strip quotes from handler name
                if (evt_handler.size() >= 2 && evt_handler.front() == '"' && evt_handler.back() == '"')
                    evt_handler = evt_handler.substr(1, evt_handler.size() - 2);
                // Check if key ends with common event suffixes
                std::string lower_key = rk;
                for (auto& c : lower_key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower_key.ends_with("changed") || lower_key.ends_with("requested")
                    || lower_key.ends_with("invoked") || lower_key.ends_with("navigated")
                    || lower_key.ends_with("navigating") || lower_key.ends_with("click")
                    || lower_key.ends_with("pressed") || lower_key.ends_with("loaded")
                    || lower_key.ends_with("entered") || lower_key.ends_with("exited")
                    || lower_key.ends_with("tapped") || lower_key.ends_with("dropped")
                    || lower_key.ends_with("moving") || lower_key.ends_with("changing")
                    || lower_key.ends_with("canceling") || lower_key.ends_with("pointer")
                    || lower_key.ends_with("hover") || lower_key.ends_with("doubletapped")
                    || lower_key.ends_with("righttapped") || lower_key.ends_with("holding")
                    || lower_key.ends_with("starting") || lower_key.ends_with("completed")
                    || lower_key.ends_with("selected") || lower_key.ends_with("itemclick")
                    || lower_key.ends_with("sizechanged") || lower_key.ends_with("orientationchanged")
                    || lower_key.ends_with("backrequested") || lower_key.ends_with("panetoggle")
                    || lower_key == "click" || lower_key == "tapped"
                    || lower_key == "doubletapped" || lower_key == "righttapped"
                    || lower_key == "checked" || lower_key == "unchecked"
                    || lower_key == "indeterminate" || lower_key == "keydown" || lower_key == "keyup"
                    || lower_key == "previewkeydown" || lower_key == "previewkeyup"
                    || lower_key == "pointerentered" || lower_key == "pointerexited"
                    || lower_key == "gotfocus" || lower_key == "lostfocus"
                    || lower_key == "loading" || lower_key == "loaded"
                    || lower_key == "valuechanged" || lower_key == "layoutupdated"
                    || lower_key == "externaltornouttabsdropping" || lower_key == "gettingfocus"
                    || lower_key == "copy" || lower_key == "paste" || lower_key == "cut"
                    || lower_key == "delete" || lower_key == "selectall")
                {
                    // Check if handler value looks like a function name (no spaces, no angle brackets, not a boolean/number literal or known property)
                    bool is_bool_or_num = (evt_handler == "True" || evt_handler == "true"
                        || evt_handler == "False" || evt_handler == "false"
                        || evt_handler == "nullptr"
                        || (!evt_handler.empty() && (std::isdigit(static_cast<unsigned char>(evt_handler.front())) || evt_handler.front() == '-')));
                    bool is_known_prop_not_evt = (lower_key == "selectsoninvoked" || lower_key == "behavioroninvoked"
                        || lower_key == "isselected" || lower_key == "isitemclickenabled"
                        || lower_key == "ischecked" || lower_key == "isenabled");
                    if (!is_bool_or_num && !is_known_prop_not_evt
                        && !evt_handler.empty() && evt_handler.find(' ') == std::string::npos
                        && evt_handler.find('<') == std::string::npos
                        && evt_handler.find('{') == std::string::npos)
                    {
                        looks_like_event = true;
                        std::string cb_name = evt_handler;
                        // Add to callbacks set so a stub is generated
                        add_callback(cb_name);
                        // Emit event subscription wrapped in lambda for delegate compatibility
                        // RoutedEventHandler(L&& lambda) accepts any callable via template ctor
                        // Strip 'on' prefix for WinUI3 C++ event projection
                        std::string ev_prop_name2 = rk;
                        if (ev_prop_name2.size() > 2 && ev_prop_name2[0] == 'o' && ev_prop_name2[1] == 'n') {
                            ev_prop_name2 = ev_prop_name2.substr(2);
                            if (!ev_prop_name2.empty()) ev_prop_name2[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ev_prop_name2[0])));
                        }
                        std::string evt_prop = ctrl_prop(n.control.c_str(), ev_prop_name2);
                        if (lower_key.find("panetoggle") != std::string::npos || lower_key == "panetogglerequested" || lower_key == "viewchanging" || lower_key == "viewchanged")
                        {
                            out << indent << var_name << ".add_" << evt_prop << "([this](auto const& s, auto const& e) { " << cb_name << "(s, e); });\n";
                        }
                        else
                        {
                            // Kept as a comment: winui files route events through
                            // .onEvent (the n.events path above); raw XAML event
                            // attributes are rare and the ends_with() heuristic
                            // misclassifies property assignments (IsSelected="True",
                            // IsChecked=...) as events, which would emit invalid
                            // add_IsSelected/... calls.
                            out << indent << "// " << var_name << ".add_" << evt_prop << "([this](auto const& s, auto const& e) { " << cb_name << "(s, e); });\n";
                        }
                    }
                }
                if (looks_like_event) continue;
            }
            // Check if rval is already a resolved C++ expression (Thickness, CornerRadius, theme lookup, etc.)
            bool is_cxx_val = (rval.find("Thickness{") != std::string::npos)
                || (rval.find("CornerRadius{") != std::string::npos)
                || (rval.find("FontWeight{") != std::string::npos)
                || (rval.find("winrt::") != std::string::npos)
                || (rval.find("theme_brush_th(") != std::string::npos)
                || (rval.find("theme_lookup_th(") != std::string::npos)
                || (rval.find("style_lookup_th(") != std::string::npos)
                || (rval.find("control_template_th(") != std::string::npos)
                || (rval.find("data_template_th(") != std::string::npos)
                || (rval.find("make_solid_brush(") != std::string::npos);
            // XAML-specific: colons (x:Name), attached props (Grid.Row), XAML markup in values
            bool is_xaml_specific = (rk.find(':') != std::string::npos) || (rk.find('.') != std::string::npos);
            bool is_xml_fragment_val = (!rval.empty() && rval.front() == '<') ||
                (rval.size() > 2 && rval[0] == 'L' && rval[1] == '"' && rval[2] == '<');
            bool has_xaml_markup = is_xml_fragment_val || (!is_cxx_val && ((rval.find('<') != std::string::npos) || (rval.find('{') != std::string::npos) || (rval.find("x:Bind") != std::string::npos)));
            if (rk == "Slot")
                continue;
            if (rk == "Xaml" || rk == "CSharp" || rk == "Code")
            {
                std::string raw_val = rv;
                if (raw_val.size() >= 2 && raw_val.front() == '"' && raw_val.back() == '"') raw_val = raw_val.substr(1, raw_val.size() - 2);
                {
                    std::string unesc;
                    unesc.reserve(raw_val.size());
                    for (size_t ui = 0; ui < raw_val.size(); ++ui) {
                        if (raw_val[ui] == '\\' && ui + 1 < raw_val.size()) {
                            char nx = raw_val[ui + 1];
                            if (nx == 'n') { unesc += '\n'; ++ui; continue; }
                            if (nx == 'r') { unesc += '\r'; ++ui; continue; }
                            if (nx == 't') { unesc += '\t'; ++ui; continue; }
                            if (nx == '"') { unesc += '"'; ++ui; continue; }
                        }
                        unesc += raw_val[ui];
                    }
                    raw_val = unesc;
                }
                if (raw_val.starts_with("{x:Bind ") || raw_val.starts_with("{Binding "))
                {
                    std::string expr = raw_val;
                    if (!expr.empty() && expr.front() == '{') expr = expr.substr(1);
                    if (!expr.empty() && expr.back() == '}') expr.pop_back();
                    auto mode_pos = expr.find(", Mode=");
                    if (mode_pos != std::string::npos) expr = expr.substr(0, mode_pos);
                    if (expr.size() > 6 && expr.substr(0, 6) == "x:Bind") expr = expr.substr(6);
                    while (!expr.empty() && expr.front() == ' ') expr = expr.substr(1);
                    std::string cpp_val = resolve_binding_to_cpp(expr, view.state_vars);
                    if (!cpp_val.empty())
                    {
                        out << indent << var_name << "." << rk << "(" << cpp_val << ");\n";
                        if (cpp_val.find('.') == std::string::npos)
                            out << indent << "m_bindings.push_back([this, " << cap_var << "]() mutable { " << var_name << "." << rk << "(" << cpp_val << "); });\n";
                    }
                }
                else
                {
                    out << indent << var_name << "." << rk << "(winrt::hstring(L\"" << escape_wstr(raw_val) << "\"));\n";
                }
                continue;
            }
            std::string lo_ctrl = n.control;
            for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string lo_rk = rk;
            for (auto& c : lo_rk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lo_ctrl == "titlebar" && lo_rk == "iconsource")
            {
                std::string icon_path = rv;
                auto p1 = icon_path.find("ImageSource=\"");
                if (p1 != std::string::npos) {
                    p1 += 13;
                    auto p2 = icon_path.find('"', p1);
                    if (p2 != std::string::npos) icon_path = icon_path.substr(p1, p2 - p1);
                }
                for (auto& c : icon_path) if (c == '\\') c = '/';
                if (!icon_path.empty() && icon_path[0] == '/') icon_path = "ms-appx:///" + icon_path.substr(1);
                else if (icon_path.rfind("Assets/", 0) == 0) icon_path = "ms-appx:///" + icon_path;
                out << indent << "{ auto __iis = winrt::Microsoft::UI::Xaml::Controls::ImageIconSource(); __iis.ImageSource(winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << icon_path << "\"))); " << var_name << ".IconSource(winrt::Microsoft::UI::Xaml::Controls::IconSource::wrap_borrowed(__iis.m_unknown.get())); }\n";
                continue;
            }
            if (lo_rk == "headericon")
            {
                out << indent << var_name << ".HeaderIcon(winrt::hstring(L\"" << escape_wstr(rv) << "\"));\n";
                continue;
            }
            if (lo_rk == "actionicon")
            {
                out << indent << var_name << ".ActionIcon(winrt::hstring(L\"" << escape_wstr(rv) << "\"));\n";
                continue;
            }
            if (is_xaml_specific || has_xaml_markup || rk == "Resources" || rk == "resources" || rk == "VisualStateGroups" || rk == "visualstategroups")
            {
                // DataTemplate XAML strings: skipped (handled by factory approach)
                if (rval.find("DataTemplate") != std::string::npos
                    && rval.find("<DataTemplate") != std::string::npos)
                {
                    // Skipped: DataTemplate handled by ChoosingItemContainer
                }
                else if (rk.find(':') == std::string::npos && rk.find('.') == std::string::npos && rval.find("x:Bind") != std::string::npos)
                {
                    // x:Bind binding on a plain property — resolve to C++ read
                    // {x:Bind expr, Mode=OneWay} → extract expr and project as getter
                    std::string expr = rval;
                    // Strip L"..." wrapper if present
                    if (expr.size() > 2 && expr[0] == 'L' && expr[1] == '"')
                        expr = expr.substr(2, expr.size() - 3);
                    else if (expr.size() > 2 && expr[0] == '"' && expr.back() == '"')
                        expr = expr.substr(1, expr.size() - 2);
                    // Strip { and } wrappers
                    if (!expr.empty() && expr.front() == '{') expr = expr.substr(1);
                    if (!expr.empty() && expr.back() == '}') expr.pop_back();
                    // Extract just the expression before Mode=
                    auto mode_pos = expr.find(", Mode=");
                    if (mode_pos != std::string::npos) expr = expr.substr(0, mode_pos);
                    // Strip x:Bind prefix
                    if (expr.size() > 6 && expr.substr(0, 6) == "x:Bind")
                        expr = expr.substr(6);
                    // Trim leading whitespace
                    while (!expr.empty() && expr.front() == ' ') expr = expr.substr(1);
                    // Guard: reject corrupt/truncated values (template JSON blobs,
                    // pre-boxed fragments, unbalanced markup) — only clean binding
                    // expressions may be projected as C++ reads.
                    bool corrupt_expr = expr.find('"') != std::string::npos
                        || expr.find('\\') != std::string::npos
                        || expr.find('{') != std::string::npos
                        || expr.find('}') != std::string::npos
                        || expr.find(':') != std::string::npos
                        || expr.find('<') != std::string::npos;
                    if (corrupt_expr || expr.empty())
                    {
                        out << indent << "// " << var_name << "." << rk << " skipped: non-projected binding/template payload\n";
                    }
                    else
                    {
                        // Resolve: rootFrame.CanGoBack → rootFrame.CanGoBack()
                        std::string cpp_val = resolve_binding_to_cpp(expr, view.state_vars);
                        // Struct-typed props bound to a numeric expression (Margin
                        // ← slider value): expand into a uniform Thickness.
                        std::string rk_lo_bind2 = rk;
                        for (auto& c : rk_lo_bind2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if ((rk_lo_bind2 == "margin" || rk_lo_bind2 == "borderthickness" || rk_lo_bind2 == "padding")
                            && !cpp_val.empty())
                            cpp_val = "winrt::Microsoft::UI::Xaml::Thickness{" + cpp_val + ", " + cpp_val + ", " + cpp_val + ", " + cpp_val + "}";
                        out << indent << var_name << "." << rk << "(" << cpp_val << ");\n";
                        out << indent << "m_bindings.push_back([this, " << cap_var << "]() mutable { " << var_name << "." << rk << "(" << cpp_val << "); });\n";
                    }
                }
                else if (rk == "Resources" || rk == "resources")
                {
                    std::string raw_val = normalize_payload_value(rval);
                    out << indent << "apply_xaml_resources(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(" << var_name << ".m_unknown.get()), L\"" << escape_wstr(raw_val) << "\");\n";
                }
                else if (rk == "VisualStateGroups" || rk == "visualstategroups")
                {
                    std::string raw_val = normalize_payload_value(rval);
                    out << indent << "apply_visual_state_groups(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(" << var_name << ".m_unknown.get()), L\"" << escape_wstr(raw_val) << "\");\n";
                }
                else if (rk == "Template" || rk == "ControlTemplate" || rk == "template")
                {
                    std::string raw_val = normalize_payload_value(rval);
                    if (raw_val.find("StaticResource ") != std::string::npos || raw_val.find("ThemeResource ") != std::string::npos)
                    {
                        auto ks = raw_val.find("Resource ") + 9;
                        auto ke = raw_val.find('}', ks);
                        if (ke != std::string::npos)
                        {
                            std::string res_key = raw_val.substr(ks, ke - ks);
                            out << indent << var_name << ".Template(control_template_th(L\"" << res_key << "\"));\n";
                        }
                    }
                    else if (!raw_val.empty() && raw_val[0] == '<')
                    {
                        out << indent << var_name << ".Template(load_control_template(L\"" << escape_wstr(raw_val) << "\"));\n";
                    }
                }
                else if (rk.rfind("AutomationProperties.", 0) == 0)
                {
                    std::string sub = rk.substr(21);
                    std::string raw_val = rval;
                    if (raw_val.size() > 2 && raw_val[0] == 'L' && raw_val[1] == '"') raw_val = raw_val.substr(2, raw_val.size() - 3);
                    else if (raw_val.size() >= 2 && raw_val.front() == '"' && raw_val.back() == '"') raw_val = raw_val.substr(1, raw_val.size() - 2);
                    std::string interp = try_interpolate_string(raw_val, view.state_vars);
                    if (sub == "Name")
                    {
                        if (!interp.empty())
                            out << indent << "AutomationProperties::SetName(" << var_name << ", " << interp << ");\n";
                        else if (raw_val.starts_with("{x:Bind ") || raw_val.starts_with("{Binding "))
                            out << indent << "// " << var_name << " AutomationProperties.Name binding skipped\n";
                        else
                            out << indent << "AutomationProperties::SetName(" << var_name << ", L\"" << escape_wstr(raw_val) << "\");\n";
                    }
                    else if (sub == "AutomationId")
                    {
                        out << indent << "AutomationProperties::SetAutomationId(" << var_name << ", L\"" << escape_wstr(raw_val) << "\");\n";
                    }
                    else if (sub == "HeadingLevel")
                    {
                        std::string hl = raw_val;
                        if (hl == "1") hl = "Level1";
                        else if (hl == "2") hl = "Level2";
                        else if (hl == "3") hl = "Level3";
                        else if (hl == "4") hl = "Level4";
                        else if (hl == "5") hl = "Level5";
                        else if (hl == "6") hl = "Level6";
                        else if (hl == "7") hl = "Level7";
                        else if (hl == "8") hl = "Level8";
                        else if (hl == "9") hl = "Level9";
                        else if (hl == "0") hl = "None";
                        out << indent << "AutomationProperties::SetHeadingLevel(" << var_name << ", winrt::Microsoft::UI::Xaml::Automation::Peers::AutomationHeadingLevel::" << hl << ");\n";
                    }
                    else if (sub == "AccessibilityView")
                    {
                        out << indent << "AutomationProperties::SetAccessibilityView(" << var_name << ", winrt::Microsoft::UI::Xaml::Automation::Peers::AccessibilityView::" << raw_val << ");\n";
                    }
                    else if (sub == "HelpText")
                    {
                        if (!interp.empty())
                            out << indent << "AutomationProperties::SetHelpText(" << var_name << ", " << interp << ");\n";
                        else if (raw_val.starts_with("{x:Bind ") || raw_val.starts_with("{Binding "))
                            out << indent << "// " << var_name << " AutomationProperties.HelpText binding skipped\n";
                        else
                            out << indent << "AutomationProperties::SetHelpText(" << var_name << ", L\"" << escape_wstr(raw_val) << "\");\n";
                    }
                    else if (sub == "LiveSetting")
                    {
                        out << indent << "AutomationProperties::SetLiveSetting(" << var_name << ", winrt::Microsoft::UI::Xaml::Automation::Peers::AutomationLiveSetting::" << raw_val << ");\n";
                    }
                    else if (sub == "AutomationControlType")
                    {
                        out << indent << "AutomationProperties::SetAutomationControlType(" << var_name << ", winrt::Microsoft::UI::Xaml::Automation::Peers::AutomationControlType::" << raw_val << ");\n";
                    }
                    else if (sub == "LandmarkType")
                    {
                        out << indent << "AutomationProperties::SetLandmarkType(" << var_name << ", winrt::Microsoft::UI::Xaml::Automation::Peers::AutomationLandmarkType::" << raw_val << ");\n";
                    }
                    else if (sub == "LocalizedControlType")
                    {
                        out << indent << "AutomationProperties::SetLocalizedControlType(" << var_name << ", L\"" << escape_wstr(raw_val) << "\");\n";
                    }
                    else if (sub == "LocalizedLandmarkType")
                    {
                        out << indent << "AutomationProperties::SetLocalizedLandmarkType(" << var_name << ", L\"" << escape_wstr(raw_val) << "\");\n";
                    }
                    else if (sub == "AcceleratorKey")
                    {
                        out << indent << "AutomationProperties::SetAcceleratorKey(" << var_name << ", L\"" << escape_wstr(raw_val) << "\");\n";
                    }
                    else if (sub == "FullDescription")
                    {
                        if (!interp.empty())
                            out << indent << "AutomationProperties::SetFullDescription(" << var_name << ", " << interp << ");\n";
                        else if (raw_val.starts_with("{x:Bind ") || raw_val.starts_with("{Binding "))
                            out << indent << "// " << var_name << " AutomationProperties.FullDescription binding skipped\n";
                        else
                            out << indent << "AutomationProperties::SetFullDescription(" << var_name << ", L\"" << escape_wstr(raw_val) << "\");\n";
                    }
                    else if (sub == "SizeOfSet")
                    {
                        out << indent << "AutomationProperties::SetSizeOfSet(" << var_name << ", " << raw_val << ");\n";
                    }
                    else if (sub == "PositionInSet")
                    {
                        out << indent << "AutomationProperties::SetPositionInSet(" << var_name << ", " << raw_val << ");\n";
                    }
                    else if (sub == "LabeledBy")
                    {
                        std::string target;
                        auto el_pos = raw_val.find("ElementName=");
                        if (el_pos != std::string::npos)
                        {
                            target = raw_val.substr(el_pos + 12);
                            auto end_pos = target.find_first_of(" }");
                            if (end_pos != std::string::npos) target = target.substr(0, end_pos);
                        }
                        else if (raw_val.find("x:Bind ") != std::string::npos)
                        {
                            target = raw_val.substr(raw_val.find("x:Bind ") + 7);
                            auto end_pos = target.find_first_of(" ,}");
                            if (end_pos != std::string::npos) target = target.substr(0, end_pos);
                        }
                        if (!target.empty())
                            out << indent << "_deferred_labeled_by.push_back({ winrt::Microsoft::UI::Xaml::DependencyObject::wrap_borrowed(" << var_name << ".m_unknown.get()), [this]() -> winrt::Microsoft::UI::Xaml::UIElement { return winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << target << ".m_unknown.get()); } });\n";
                    }
                }
                else if (rk == "ToolTipService.ToolTip")
                {
                    std::string raw_val = rval;
                    if (raw_val.size() > 2 && raw_val[0] == 'L' && raw_val[1] == '"') raw_val = raw_val.substr(2, raw_val.size() - 3);
                    else if (raw_val.size() >= 2 && raw_val.front() == '"' && raw_val.back() == '"') raw_val = raw_val.substr(1, raw_val.size() - 2);
                    std::string interp = try_interpolate_string(raw_val, view.state_vars);
                    if (!interp.empty())
                        out << indent << "ToolTipService::SetToolTip(" << var_name << ", winrt::box_value(" << interp << "));\n";
                    else if (raw_val.starts_with("{x:Bind ") || raw_val.starts_with("{Binding "))
                        out << indent << "// " << var_name << " ToolTipService.ToolTip binding skipped\n";
                    else
                        out << indent << "ToolTipService::SetToolTip(" << var_name << ", winrt::box_value(winrt::hstring(L\"" << escape_wstr(raw_val) << "\")));\n";
                }
                else if (rk.rfind("RelativePanel.", 0) == 0)
                {
                    std::string prop = rk.substr(14);
                    std::string raw_val = rv;
                    if (raw_val.size() >= 2 && raw_val.front() == '"' && raw_val.back() == '"') raw_val = raw_val.substr(1, raw_val.size() - 2);
                    if (prop.find("WithPanel") != std::string::npos)
                    {
                        bool b = (raw_val == "True" || raw_val == "true" || raw_val == "1");
                        out << indent << "winrt::Microsoft::UI::Xaml::Controls::RelativePanel::Set" << prop << "(" << var_name << ", " << (b ? "true" : "false") << ");\n";
                    }
                    else if (named_elements.count(raw_val))
                    {
                        out << indent << "_deferred_relative_panel.push_back([this, " << var_name << "]() { if (" << var_name << ".m_unknown.get() && " << raw_val << ".m_unknown.get()) winrt::Microsoft::UI::Xaml::Controls::RelativePanel::Set" << prop << "(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(" << var_name << ".m_unknown.get()), winrt::IInspectable(" << raw_val << ".addref_borrowed())); });\n";
                    }
                }
                else if (rk == "Header")
                {
                    auto t1 = rval.find("Text=\\\"");
                    if (t1 != std::string::npos)
                    {
                        t1 += 7;
                        auto t2 = rval.find("\\\"", t1);
                        if (t2 != std::string::npos)
                        {
                            std::string hdr_txt = rval.substr(t1, t2 - t1);
                            out << indent << var_name << ".Header(winrt::box_value(winrt::hstring(L\"" << escape_wstr(hdr_txt) << "\")));\n";
                        }
                    }
                    else
                    {
                        t1 = rval.find("Text=\"");
                        if (t1 != std::string::npos)
                        {
                            t1 += 6;
                            auto t2 = rval.find('"', t1);
                            if (t2 != std::string::npos)
                            {
                                std::string hdr_txt = rval.substr(t1, t2 - t1);
                                out << indent << var_name << ".Header(winrt::box_value(winrt::hstring(L\"" << escape_wstr(hdr_txt) << "\")));\n";
                            }
                        }
                    }
                }
                else if (rk == "String" || rv.find("<x:String>") != std::string::npos || rv.find("<sys:String>") != std::string::npos)
                {
                    auto s1 = rv.find('>');
                    auto s2 = rv.rfind('<');
                    if (s1 != std::string::npos && s2 != std::string::npos && s2 > s1)
                    {
                        std::string item_str = rv.substr(s1 + 1, s2 - s1 - 1);
                        {
                            std::string unesc;
                            unesc.reserve(item_str.size());
                            for (size_t ui = 0; ui < item_str.size(); ++ui) {
                                if (item_str[ui] == '\\' && ui + 1 < item_str.size()) {
                                    char nx = item_str[ui + 1];
                                    if (nx == 'n') { unesc += '\n'; ++ui; continue; }
                                    if (nx == 'r') { unesc += '\r'; ++ui; continue; }
                                    if (nx == 't') { unesc += '\t'; ++ui; continue; }
                                    if (nx == '"') { unesc += '"'; ++ui; continue; }
                                }
                                unesc += item_str[ui];
                            }
                            item_str = unesc;
                        }
                        if (n.control == "SampleCodePresenter")
                            out << indent << var_name << ".Code(winrt::hstring(L\"" << escape_wstr(item_str) << "\"));\n";
                        else if (n.control == "ControlExample")
                            out << indent << var_name << ".Xaml(winrt::hstring(L\"" << escape_wstr(item_str) << "\"));\n";
                        else
                            out << indent << "[&](auto& __v) { if constexpr (requires { __v.Items(); }) { __v.Items().Append(winrt::box_value(winrt::hstring(L\"" << escape_wstr(item_str) << "\"))); } }(" << var_name << ");\n";
                    }
                }
                else
                {
                    // Non-compilable XAML attribute skipped (Grid.Row, x:Name, etc.)
                }
            }
            else
            {
                // Special: .Icon("Home") -> .Icon(SymbolIcon(Symbol::Home))
                if (rk == "Icon")
                {
                    std::string icon_val = rval;
                    if (icon_val.size() > 3 && icon_val[0] == 'L' && icon_val[1] == '"')
                        icon_val = icon_val.substr(2, icon_val.size() - 3);
                    else if (icon_val.size() > 2 && icon_val[0] == '"')
                        icon_val = icon_val.substr(1, icon_val.size() - 2);
                    if (icon_val.size() >= 2 && icon_val.front() == '"' && icon_val.back() == '"')
                        icon_val = icon_val.substr(1, icon_val.size() - 2);
                    // Skip bool/expr values that are not valid Symbol enum names
                    bool is_bad_icon_val = (icon_val == "true" || icon_val == "false" || icon_val == "nullptr"
                        || icon_val.find("::") != std::string::npos
                        || icon_val.find("box_value") != std::string::npos
                        || icon_val.find("from_abi") != std::string::npos);
                    // Projected types are standalone structs: Icon takes IconElement,
                    // so route the constructed SymbolIcon through from_abi with a
                    // scoped temp (same pattern as the element-child icon paths).
                    if (is_bad_icon_val)
                        out << indent << "{ auto __ic = SymbolIcon(); " << var_name << ".Icon(IconElement::wrap_borrowed(__ic.m_unknown.get())); }\n";
                    else if (!icon_val.empty() && icon_val[0] != '{')
                        out << indent << "{ auto __ic = SymbolIcon(symbol_from_name(L\"" << icon_val << "\")); " << var_name << ".Icon(IconElement::wrap_borrowed(__ic.m_unknown.get())); }\n";
                    else
                        out << indent << "{ auto __ic = SymbolIcon(); " << var_name << ".Icon(IconElement::wrap_borrowed(__ic.m_unknown.get())); }\n";
                }
                else
                {
                    // WebView2.Source takes a Uri
                    if (rk == "Source" && n.control == "WebView2")
                    {
                        std::string wv_url = rval;
                        if (wv_url.size() > 3 && wv_url[0] == 'L' && wv_url[1] == '\"')
                            wv_url = wv_url.substr(2, wv_url.size() - 3);
                        else if (wv_url.size() > 2 && wv_url[0] == '\"')
                            wv_url = wv_url.substr(1, wv_url.size() - 2);
                        if (wv_url.size() >= 2 && wv_url.front() == '\"' && wv_url.back() == '\"')
                            wv_url = wv_url.substr(1, wv_url.size() - 2);
                        // strip trailing () artifact from parsed attribute values
                        while (wv_url.size() >= 2 && wv_url.back() == ')' && wv_url[wv_url.size() - 2] == '(')
                            wv_url.pop_back(), wv_url.pop_back();
                        out << indent << var_name << ".Source(winrt::Windows::Foundation::Uri(L\"" << wv_url << "\"));\n";
                    }
                    // Normalize Image/ImageIcon Source paths
                    else if (rk == "Source" && (n.control == "Image" || n.control == "ImageIcon" || n.control == "BitmapIcon"))
                    {
                        std::string img_val = rval;
                        // Strip string wrapper
                        if (img_val.size() > 3 && img_val[0] == 'L' && img_val[1] == '"' )
                            img_val = img_val.substr(2, img_val.size() - 3);
                        else if (img_val.size() > 2 && img_val[0] == '"' )
                            img_val = img_val.substr(1, img_val.size() - 2);
                        if (img_val.size() >= 2 && img_val.front() == '"' && img_val.back() == '"' )
                            img_val = img_val.substr(1, img_val.size() - 2);
                        for (auto& c : img_val) if (c == '\\') c = '/';
                        if (img_val.find("{ThemeResource ") != std::string::npos || img_val.find("{StaticResource ") != std::string::npos || (img_val.find('/') == std::string::npos && img_val.find('.') == std::string::npos))
                        {
                            std::string res_key = img_val;
                            auto p = res_key.find("Resource ");
                            if (p != std::string::npos) {
                                res_key = res_key.substr(p + 9);
                                if (!res_key.empty() && res_key.back() == '}') res_key.pop_back();
                                while (!res_key.empty() && res_key.front() == ' ') res_key.erase(0, 1);
                                while (!res_key.empty() && res_key.back() == ' ') res_key.pop_back();
                            }
                            out << indent << var_name << ".Source(image_source_lookup_th(L\"" << escape_wstr(res_key) << "\"));\n";
                        }
                        else
                        {
                            if (!img_val.empty() && img_val[0] == '/')
                                img_val = "ms-appx:///" + img_val.substr(1);
                            else if (img_val.rfind("Assets/", 0) == 0)
                                img_val = "ms-appx:///" + img_val;
                            out << indent << var_name << ".Source(winrt::Microsoft::UI::Xaml::Media::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << img_val << "\")));\n";
                        }
                    }
                    // ImageIconSource.Source/ImageSource(L"url") -> BitmapImage(Uri)
                    else if ((rk == "Source" || rk == "ImageSource") && n.control == "ImageIconSource")
                    {
                        std::string img_val2 = rval;
                        if (img_val2.size() > 3 && img_val2[0] == 'L' && img_val2[1] == '"')
                            img_val2 = img_val2.substr(2, img_val2.size() - 3);
                        else if (img_val2.size() > 2 && img_val2[0] == '"')
                            img_val2 = img_val2.substr(1, img_val2.size() - 2);
                        if (img_val2.size() >= 2 && img_val2.front() == '"' && img_val2.back() == '"')
                            img_val2 = img_val2.substr(1, img_val2.size() - 2);
                        for (auto& c : img_val2) if (c == '\\') c = '/';
                        if (img_val2.find("{ThemeResource ") != std::string::npos || img_val2.find("{StaticResource ") != std::string::npos || (img_val2.find('/') == std::string::npos && img_val2.find('.') == std::string::npos))
                        {
                            std::string res_key = img_val2;
                            auto p = res_key.find("Resource ");
                            if (p != std::string::npos) {
                                res_key = res_key.substr(p + 9);
                                if (!res_key.empty() && res_key.back() == '}') res_key.pop_back();
                                while (!res_key.empty() && res_key.front() == ' ') res_key.erase(0, 1);
                                while (!res_key.empty() && res_key.back() == ' ') res_key.pop_back();
                            }
                            if (rk == "ImageSource")
                                out << indent << var_name << ".ImageSource(image_source_lookup_th(L\"" << escape_wstr(res_key) << "\"));\n";
                            else
                                out << indent << var_name << ".Source(image_source_lookup_th(L\"" << escape_wstr(res_key) << "\"));\n";
                        }
                        else
                        {
                            if (!img_val2.empty() && img_val2[0] == '/')
                                img_val2 = "ms-appx:///" + img_val2.substr(1);
                            else if (img_val2.rfind("Assets/", 0) == 0)
                                img_val2 = "ms-appx:///" + img_val2;
                            if (rk == "ImageSource")
                                out << indent << var_name << ".ImageSource(winrt::Microsoft::UI::Xaml::Media::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << img_val2 << "\")));\n";
                            else
                                out << indent << var_name << ".Source(winrt::Microsoft::UI::Xaml::Media::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << img_val2 << "\")));\n";
                        }
                    }
                    // SymbolIconSource(Symbol::X) -> Symbol enum not projected; the
                    // self-named .SymbolIconSource(SymbolIconSource()) call is invalid.
                    else if (rk == "Symbol" && n.control == "SymbolIconSource")
                    {
                        out << indent << "// " << var_name << ".Symbol skipped: SymbolIconSource.Symbol (Symbol enum not projected)\n";
                    }
                    // QueryIcon -> needs IconElement, not SymbolIconSource.
                    // IconElement is ABSTRACT: default-constructing it calls
                    // create_instance on a non-activatable class -> null factory
                    // -> access violation at startup. Build a concrete SymbolIcon
                    // from the raw glyph name and wrap it as IconElement.
                    else if (rk == "QueryIcon" || rk == "queryicon")
                    {
                        std::string sym_val = rval;
                        if (sym_val.size() > 3 && sym_val[0] == 'L' && sym_val[1] == '"')
                            sym_val = sym_val.substr(2, sym_val.size() - 3);
                        else if (sym_val.size() > 2 && sym_val[0] == '"')
                            sym_val = sym_val.substr(1, sym_val.size() - 2);
                        if (sym_val.size() >= 2 && sym_val.front() == '"' && sym_val.back() == '"')
                            sym_val = sym_val.substr(1, sym_val.size() - 2);
                        bool is_bad_sym = (sym_val.empty() || sym_val == "true" || sym_val == "false" || sym_val == "nullptr"
                            || sym_val.find("::") != std::string::npos
                            || sym_val.find("box_value") != std::string::npos
                            || sym_val.find("from_abi") != std::string::npos);
                        if (is_bad_sym)
                            out << indent << "{ auto __ic = SymbolIcon(); " << var_name << ".QueryIcon(winrt::Microsoft::UI::Xaml::Controls::IconElement::wrap_borrowed(__ic.m_unknown.get())); }\n";
                        else
                            out << indent << "{ auto __ic = SymbolIcon(symbol_from_name(L\"" << sym_val << "\")); " << var_name << ".QueryIcon(winrt::Microsoft::UI::Xaml::Controls::IconElement::wrap_borrowed(__ic.m_unknown.get())); }\n";
                    }
                    // Special: .Symbol("Settings") -> .Symbol(symbol_from_name(L"Settings"))
                else if (rk == "Symbol")
                {
                    std::string sym_val = rval;
                    if (sym_val.size() > 3 && sym_val[0] == 'L' && sym_val[1] == '"')
                        sym_val = sym_val.substr(2, sym_val.size() - 3);
                    else if (sym_val.size() > 2 && sym_val[0] == '"')
                        sym_val = sym_val.substr(1, sym_val.size() - 2);
                    if (sym_val.size() >= 2 && sym_val.front() == '"' && sym_val.back() == '"')
                        sym_val = sym_val.substr(1, sym_val.size() - 2);
                    // Skip bool/expr values that are not valid Symbol enum names
                    bool is_bad_sym = (sym_val == "true" || sym_val == "false" || sym_val == "nullptr"
                        || sym_val.find("::") != std::string::npos
                        || sym_val.find("box_value") != std::string::npos
                        || sym_val.find("from_abi") != std::string::npos);
                    if (is_bad_sym)
                        out << indent << var_name << ".Symbol(symbol_from_name(L\"None\"));\n";
                    else if (!sym_val.empty() && sym_val[0] != '{')
                        out << indent << var_name << ".Symbol(symbol_from_name(L\"" << sym_val << "\"));\n";
                    else
                        out << indent << var_name << ".Symbol(symbol_from_name(L\"None\"));\n";
                }
                else
                    {
                        // Check if this is a brush property with a hex color value
                        std::string rk_lo3 = rk;

                        for (auto& c : rk_lo3) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                        // Strip L"..." wrapper from rval before hex check

                        std::string hv = rval;

                        if (hv.size() > 3 && hv[0] == 'L' && hv[1] == '"' ) hv = hv.substr(2, hv.size() - 3);

                        else if (hv.size() > 2 && hv[0] == '"' && hv.back() == '"' ) hv = hv.substr(1, hv.size() - 2);                        // Try CSS color formats first (hsl, oklch, display-p3)
                        std::string css_color = try_css_color(rval);
                        if (!css_color.empty())
                        {
                            bool is_brush = is_brush_property(rk_lo3);
                            std::string ca = is_brush
                                ? "make_solid_brush(" + css_color + ")"
                                : css_color;
                            out << indent << var_name << "." << raw_prop_name(rk) << "(" << ca << ");\n";
                        }
                        else if (hv.size() >= 7 && hv[0] == '#')
                        {
                            bool is_brush = is_brush_property(rk_lo3);
                            if (is_brush) {
                                std::string brush_ctor = "make_solid_brush(winrt::Windows::UI::Color{";
                                if (hv.size() == 9)
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(make_solid_brush(winrt::Windows::UI::Color{" << hex_to_argb(hv) << "}));\n";
                                else if (hv.size() == 7)
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(make_solid_brush(winrt::Windows::UI::Color{255, " << hex_to_rgb(hv) << "}));\n";
                                else
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(" << box_if_inspectable(n.control, rk, rval) << ");\n";
                            } else {
                                std::string ca = "winrt::Windows::UI::Color{";
                                if (hv.size() == 9)
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(" << ca << hex_to_argb(hv) << "});\n";
                                else if (hv.size() == 7)
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(" << ca << "255, " << hex_to_rgb(hv) << "});\n";
                                else
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(" << box_if_inspectable(n.control, rk, rval) << ");\n";
                            }
                        }
                        else
                        {
                            // Skip properties that take non-string types (Geometry, Brush, FontFamily)
                            // when value is empty — these can't be constructed from L""
                            std::string rk_lo_skip = rk;
                            for (auto& c : rk_lo_skip) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            bool skip_empty = (rk_lo_skip == "data" || rk_lo_skip == "foreground" || rk_lo_skip == "fontfamily" || rk_lo_skip == "iconsource" || rk_lo_skip == "ticks" || rk_lo_skip == "rowdefinitions" || rk_lo_skip == "columndefinitions" || rk_lo_skip == "inlines");
                            if (skip_empty && rval.size() <= 4) {} // skip L"" or empty
                            else
                            {
                                // If rval is already a valid C++ expression, emit as-is.
                                // Otherwise, detect bare string/unicode values and wrap in L"...".
                                // NOTE: parenthesized text like "All Files (*)" or "Period (in ms)"
                                // must NOT count as an expression - only parens without spaces may.
                                std::string emit_val = rval;
                                std::string interp = try_interpolate_string(rval, view.state_vars);
                                if (!interp.empty())
                                {
                                    emit_val = interp;
                                    // Property-aware conversion: struct/enum-typed
                                    // setters cannot take a raw string state var —
                                    // wrap with the by-name helpers.
                                    std::string rk_lo_sv = rk;
                                    for (auto& c : rk_lo_sv) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (rk_lo_sv == "verticalalignment")
                                        emit_val = "valignment_from_name(" + emit_val + ")";
                                    else if (rk_lo_sv == "horizontalalignment")
                                        emit_val = "halignment_from_name(" + emit_val + ")";
                                    else if (rk_lo_sv == "borderthickness" || rk_lo_sv == "margin" || rk_lo_sv == "padding")
                                        emit_val = "thickness_from_name(" + emit_val + ")";
                                    else if (rk_lo_sv == "cornerradius")
                                        emit_val = "corner_from_name(" + emit_val + ")";
                                    else if (rk_lo_sv == "style")
                                        emit_val = "style_from_name(" + emit_val + ")";
                                }
                                bool has_balanced_parens_no_space = (emit_val.find('(') != std::string::npos && emit_val.find(')') != std::string::npos
                                    && emit_val.find(' ') == std::string::npos);
                                bool is_cxx_expr = has_balanced_parens_no_space
                                    || emit_val.find("::") != std::string::npos
                                    || emit_val == "true" || emit_val == "false" || emit_val == "nullptr"
                                    || emit_val.find("box_value") != std::string::npos
                                    || emit_val.find("make_solid_brush") != std::string::npos
                                    || emit_val.find("Brush::from_abi") != std::string::npos
                                    || emit_val.find("Thickness{") != std::string::npos
                                    || emit_val.find("CornerRadius{") != std::string::npos
                                    || emit_val.find("FontWeight{") != std::string::npos
                                    || emit_val.find("style_lookup_th(") != std::string::npos
                                    || emit_val.find("theme_lookup_th(") != std::string::npos
                                    || emit_val.find("theme_brush_th(") != std::string::npos
                                    || !interp.empty();
                                // JSON content: starts with " and ":", or starts with { or <
                                bool is_json_content = (emit_val.size() > 1 && emit_val.front() == '"' && emit_val.find("\":") != std::string::npos)
                                    || (!emit_val.empty() && (emit_val.front() == '{' || emit_val.front() == '<' || emit_val.front() == '['));
                                if (is_json_content && interp.empty()) is_cxx_expr = false;
                                if (!is_cxx_expr && !emit_val.empty())
                                {
                                    bool already_wstr = (emit_val.size() > 2 && emit_val[0] == 'L' && emit_val[1] == '"');
                                    bool already_str = (emit_val.size() >= 2 && emit_val.front() == '"' && emit_val.back() == '"');
                                    if (!already_wstr && !already_str)
                                    {
                                        // Detect bare string values: contains space, or is non-ASCII
                                        bool has_space = emit_val.find(' ') != std::string::npos;
                                        bool has_non_ascii = false;
                                        bool is_numeric = !emit_val.empty() && (std::isdigit(static_cast<unsigned char>(emit_val[0])) || emit_val[0] == '-');
                                        for (unsigned char uc : emit_val) {
                                            if (uc >= 0x80) { has_non_ascii = true; break; }
                                        }
                                        if (has_space || has_non_ascii)
                                            emit_val = wstr_literal(emit_val);
                                        else if (is_numeric)
                                            emit_val = emit_val; // bare number — keep as-is
                                        else
                                            emit_val = wstr_literal(emit_val); // bare identifier-like string (e.g. "Clear cache on exit")
                                    }
                                }
                                // Control-specific: TitleBar.Title needs IInspectable box_value
                                std::string lo_ctrl = n.control;
                                for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                std::string lo_rk = rk;
                                for (auto& c : lo_rk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                bool skip_binding_push = false;
                                if (lo_rk == "layout")
                                {
                                    // Layout resources (MyFeedLayout etc.) have
                                    // no projected factory here - skipping keeps
                                    // the repeater's default stack layout.
                                    out << indent << "// " << var_name << ".Layout skipped: resource-based layout\n";
                                    skip_binding_push = true;
                                }
                                else if (is_numeric_prop(lo_ctrl, lo_rk))
                                {
                                    // numeric DP: only real numbers or valid C++ expressions may be emitted, never L"..." strings
                                    std::string num_txt = strip_wstr(emit_val);
                                    if (is_number_text(num_txt))
                                        out << indent << var_name << "." << raw_prop_name(rk) << "(" << resolve_typed_value(n.control, rk, num_txt, &g_type_prop_kinds, &g_enum_info) << ");\n";
                                    else if (is_cxx_expr && interp.empty())
                                        out << indent << var_name << "." << raw_prop_name(rk) << "(" << resolve_typed_value(n.control, rk, emit_val, &g_type_prop_kinds, &g_enum_info) << ");\n";
                                    else
                                    {
                                        // String state var on a numeric DP: no C++
                                        // conversion exists - skip both the call and
                                        // its deferred binding (hstring -> int32_t is
                                        // a compile error otherwise).
                                        out << indent << "// " << var_name << "." << raw_prop_name(rk) << " skipped: non-numeric value on numeric property\n";
                                        skip_binding_push = true;
                                    }
                                }
                                else if (lo_ctrl == "titlebar" && lo_rk == "title")
                                {
                                    // TitleBar.Title takes hstring — pass the literal directly
                                    out << indent << var_name << ".Title(" << emit_val << ")";
                                }
else if (lo_ctrl == "comboboxitem" && lo_rk == "content")
                                {
                                    // ComboBoxItem.Content takes IInspectable — box strings
                                    out << indent << var_name << ".Content(winrt::box_value(" << emit_val << "))";
                                }
                                else if (lo_ctrl == "togglebutton" && lo_rk == "content")
                                {
                                    // ToggleButton.Content takes IInspectable
                                    out << indent << var_name << ".Content(winrt::box_value(" << emit_val << "))";
                                }
                                else if (lo_rk == "itemssource" && emit_val == "nullptr")
                                {
                                    // ItemsSource(nullptr) — use IInspectable nullptr
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(winrt::IInspectable{nullptr})";
                                }
                                else if (lo_ctrl == "textblock" && lo_rk == "style")
                                {
                                    // Apply standard WinUI 3 typography fallback directly
                                    if (emit_val.find("DisplayTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(68);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (emit_val.find("TitleLargeTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(40);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (emit_val.find("TitleTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(28);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (emit_val.find("SubtitleTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(20);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (emit_val.find("BodyStrongTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(14);\n";
                                        out << indent << var_name << ".FontWeight(winrt::Windows::UI::Text::FontWeight{600});\n";
                                    }
                                    else if (emit_val.find("CaptionTextBlockStyle") != std::string::npos)
                                    {
                                        out << indent << var_name << ".FontSize(12);\n";
                                    }
                                    out << indent << var_name << ".Style(" << emit_val << ")";
                                }
                                else if (lo_rk == "rendertransformorigin")
                                {
                                    std::string s_val = emit_val;
                                    if (s_val.size() > 2 && s_val[0] == 'L' && s_val[1] == '"') s_val = s_val.substr(2, s_val.size() - 3);
                                    else if (s_val.size() >= 2 && s_val.front() == '"' && s_val.back() == '"') s_val = s_val.substr(1, s_val.size() - 2);
                                    float px = 0.5f, py = 0.5f;
                                    auto comma = s_val.find(',');
                                    if (comma != std::string::npos) {
                                        try {
                                            px = std::stof(s_val.substr(0, comma));
                                            py = std::stof(s_val.substr(comma + 1));
                                        } catch (...) {}
                                    }
                                    out << indent << var_name << ".RenderTransformOrigin(winrt::Windows::Foundation::Point{" << px << "f, " << py << "f})";
                                }
                                else
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(" << box_if_inspectable(n.control, rk, resolve_typed_value(n.control, rk, emit_val, &g_type_prop_kinds, &g_enum_info)) << ")";
                                out << ";\n";
                                if (!interp.empty() && !skip_binding_push)
                                {
                                    out << indent << "m_bindings.push_back([this, " << cap_var << "]() mutable { " << var_name << "." << raw_prop_name(rk) << "(" << box_if_inspectable(n.control, rk, resolve_typed_value(n.control, rk, emit_val, &g_type_prop_kinds, &g_enum_info)) << "); });\n";
                                }
                            }
                        }
                    }
                }
            }
        }
        // (numeric-DP guard emits its own ; above)

        // Compose children — all via catalog, zero hardcoded control names
        // Track both variable names and control types for correct routing
        std::vector<std::string> child_vars;
        std::vector<std::string> child_ctrls;
        std::vector<std::string> child_slots;
        // Children inherit the enclosing panel chain; panel-like nodes
        // extend it so conditional subtrees can attach to a real parent.
        std::vector<std::string> child_chain = parent_chain;
        if (n.control == "StackPanel" || n.control == "Grid" || n.control == "Canvas"
            || n.control == "RelativePanel" || n.control == "WrapPanel"
            || n.control == "Column" || n.control == "Row"
            || n.control == "NavigationView" || n.control == "ScrollViewer" || n.control == "ScrollView"
            || n.control == "Border")
            child_chain.push_back(var_name);
        bool is_known_panel = (n.control == "StackPanel" || n.control == "Grid" || n.control == "Canvas"
            || n.control == "RelativePanel" || n.control == "WrapPanel"
            || n.control == "Column" || n.control == "Row" || n.control == "Stack");
        for (auto const& ch : n.children)
        {
            std::string cv = self(self, ch, depth + 1, child_chain);
            if (!cv.empty())
            {
                if (ch.control == "KeyboardAccelerator")
                    out << indent << "inspectable_append(" << var_name << ".KeyboardAccelerators(), " << cv << ");\n";
                else if (ch.control == "ImageIconSource")
                    out << indent << var_name << ".IconSource(winrt::Microsoft::UI::Xaml::Controls::IconSource::wrap_borrowed(" << cv << ".m_unknown.get()));\n";
                else if (is_known_panel)
                {
                    if (ch.control == "ThemeShadow")
                        out << indent << "[&](auto& __v) { if constexpr (requires { __v.Shadow(" << cv << "); }) { __v.Shadow(" << cv << "); } }(" << var_name << ");\n";
                    else if (ch.control == "CollectionViewSource")
                    {
                        // DependencyObject resource, not a UIElement child
                    }
                    else if (ch.control == "MenuFlyout" || ch.control == "CommandBarFlyout" || ch.control == "Flyout")
                    {
                        // FlyoutBase, not a UIElement child
                    }
                    else
                        out << indent << var_name << ".Children().Append(" << cv << ");\n";
                }
                else
                {
                    std::string ch_slot;
                    for (auto const& [rpk, rpv] : ch.raw_attrs)
                    {
                        if (rpk == "Slot")
                        {
                            ch_slot = rpv;
                            if (ch_slot.size() > 2 && ch_slot[0] == 'L' && ch_slot[1] == '"') ch_slot = ch_slot.substr(2, ch_slot.size() - 3);
                            else if (ch_slot.size() >= 2 && ch_slot.front() == '"' && ch_slot.back() == '"') ch_slot = ch_slot.substr(1, ch_slot.size() - 2);
                            break;
                        }
                    }
                    child_vars.push_back(std::move(cv));
                    child_ctrls.push_back(ch.control);
                    child_slots.push_back(std::move(ch_slot));
                }
            }
        }

        if (!child_vars.empty())
        {
            // First: route known special child types to their property setters
            // (KeyboardAccelerator → .KeyboardAccelerators(), ImageIconSource → .IconSource(), ThemeShadow → .Shadow(), Flyout → .Flyout())
            std::vector<std::string> remaining_vars;
            std::vector<std::string> remaining_ctrls;
            std::vector<std::string> remaining_slots;
            for (size_t i = 0; i < child_ctrls.size(); ++i)
            {
                if (child_ctrls[i] == "KeyboardAccelerator")
                    out << indent << "inspectable_append(" << var_name << ".KeyboardAccelerators(), " << child_vars[i] << ");\n";
                else if (child_ctrls[i] == "ImageIconSource")
                    out << indent << var_name << ".IconSource(winrt::Microsoft::UI::Xaml::Controls::IconSource::wrap_borrowed(" << child_vars[i] << ".m_unknown.get()));\n";
                else if (child_ctrls[i] == "ThemeShadow")
                    out << indent << "[&](auto& __v) { if constexpr (requires { __v.Shadow(" << child_vars[i] << "); }) { __v.Shadow(" << child_vars[i] << "); } }(" << var_name << ");\n";
                else if (child_ctrls[i] == "MenuFlyout" || child_ctrls[i] == "CommandBarFlyout" || child_ctrls[i] == "Flyout")
                    out << indent << "[&](auto& __v) { if constexpr (requires { __v.Flyout(" << child_vars[i] << "); }) { __v.Flyout(" << child_vars[i] << "); } }(" << var_name << ");\n";
                else if (child_ctrls[i] == "CollectionViewSource")
                {
                    // DependencyObject, not a UIElement child
                }
                else
                {
                    remaining_vars.push_back(child_vars[i]);
                    remaining_ctrls.push_back(child_ctrls[i]);
                    remaining_slots.push_back(i < child_slots.size() ? child_slots[i] : "");
                }
            }
            if (remaining_vars.empty()) { return var_name; } // all children routed
            child_vars = std::move(remaining_vars);
            child_ctrls = std::move(remaining_ctrls);
            child_slots = std::move(remaining_slots);

            // Metadata first: the winuic-emitted catalog carries composition
            // for every control — built-in, third-party and app alike. The
            // name-based chain below is only the fallback for names the
            // catalog does not know (legacy winmds).
            std::string ck = catalog.composition_kind(n.control);
            // Fallback composition_kind for controls without catalog entry
            if (ck.empty())
            {
                std::string lo = n.control;
                for (auto& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                // Documents types: children go into Inlines()
                if (lo == "paragraph" || lo == "span" || lo == "hyperlink" || lo == "bold" || lo == "italic" || lo == "underline" || lo == "linebreak")
                    ck = "documents";
                // RichTextBlock: children go into Blocks()
                else if (lo == "richtextblock")
                    ck = "blocks";
                // NavigationView: first child Content, rest MenuItems
                else if (lo == "navigationview")
                    ck = "navigation";
                // Items-based controls
                else if (lo == "radiobuttons" || lo == "selectorbar" || lo == "commandbar" || lo == "menusubitem" || lo == "menuflyoutsubitem" || lo == "splitmenuflyoutitem" || lo == "swipeitems" || lo == "swipecontrol" || lo == "tabview" || lo == "listview" || lo == "gridview" || lo == "flipview" || lo == "combobox" || lo == "pivot" || lo == "itemsview" || lo == "itemsrepeater" || lo == "treeview")
                    ck = "items";
                // Flyout/Menu items go into MenuItems()
                else if (lo == "menubaritem" || lo == "menubar" || lo == "menuflyout" || lo == "commandbarflyout" || lo == "menulayer" || lo == "submenuflyout")
                    ck = "items_panel";
                else if (lo == "navigationviewitem")
                    ck = "navigation_item";
                // Tabular TableView: columns live in Columns(), not Children()
                else if (lo == "tableview")
                    ck = "columns";
                // Charts: children route to Data()/Axes()/Series() by type
                else if (lo == "chart")
                    ck = "charts";
                // Single-child content controls
                else if (lo == "border" || lo == "card" || lo == "tabviewitem" || lo == "titlebar" || lo == "expander" || lo == "teachingtip" || lo == "splitview" || lo == "semanticzoom" || lo == "animatedicon" || lo == "togglesplitbutton" || lo == "textblock" || lo == "button" || lo == "togglebutton" || lo == "radiobutton" || lo == "hyperlinkbutton" || lo == "appbarbutton" || lo == "appbartogglebutton" || lo == "menuflyoutitem" || lo == "infobar" || lo == "infobadge" || lo == "tooltip" || lo == "image" || lo == "swipeitem" || lo == "slider" || lo == "scrollviewer" || lo == "scrollview" || lo == "splitbutton" || lo == "dropdownbutton" || lo == "viewbox" || lo == "pivotitem" || lo == "personpicture" || lo == "progressring" || lo == "progressbar" || lo == "loadingbutton" || lo == "colorpicker" || lo == "datepicker" || lo == "timespicker" || lo == "numberbox" || lo == "textbox" || lo == "passwordbox" || lo == "richeditbox" || lo == "ratingcontrol" || lo == "treeviewitem" || lo == "checkerboard" || lo == "refreshcontainer" || lo == "parallaxview" || lo == "mediaelement" || lo == "mediaplayerelement" || lo == "imageex" || lo == "webview2" || lo == "flyout" || lo == "toggleswitch" || lo == "infosecbutton" || lo == "switchpresenter" || lo == "contentpresenter")
                    ck = "content";
                // Popup: single child via .Child(), not .Children()
                else if (lo == "popup")
                    ck = "popup";
                else
                    ck = "panel";
            }
            // Platform panels always use .Children().Append(), override catalog composition_kind
            if (n.control == "StackPanel" || n.control == "Grid" || n.control == "Canvas" || n.control == "RelativePanel" || n.control == "WrapPanel" || n.control == "Column" || n.control == "Row")
                ck = "panel";
            // ComboBox/ListView/Pivot/CommandBar always use .Items() or .PrimaryCommands()
            if ((n.control == "Pivot" || n.control == "ComboBox" || n.control == "ListView" || n.control == "GridView" || n.control == "FlipView" || n.control == "CommandBar" || n.control == "CommandBarFlyout") && ck != "items")
                ck = "items";
            // Detect icon children for .Icon() routing
            bool has_icon_child = false;
            for (auto const& ch : n.children)
                if (is_icon_element(ch.control)) { has_icon_child = true; break; }

            if (ck == "items")
            {
                std::string coll_prop = "Items";
                std::string lo_ctrl(n.control);
                for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lo_ctrl == "menubar") coll_prop = "Items";
                else if (lo_ctrl == "menubaritem") coll_prop = "Items";
                else if (lo_ctrl == "menuflyout") coll_prop = "Items";
                else if (lo_ctrl == "flyout") coll_prop = "Items";
                else if (lo_ctrl == "commandbar") coll_prop = "PrimaryCommands";
                else if (lo_ctrl == "commandbarflyout") coll_prop = "PrimaryCommands";
                else if (lo_ctrl == "tabview") coll_prop = "TabItems";
                else if (lo_ctrl == "swipecontrol") coll_prop = "SwipeItems";
                else if (lo_ctrl == "swipeitems") {
                    // SwipeItems IS the collection: append directly
                    for (auto const& cv : child_vars)
                        out << indent << "inspectable_append(" << var_name << ", " << cv << ");\n";
                    return var_name;
                }
                else if (lo_ctrl == "itemsrepeater" || lo_ctrl == "itemsview" || lo_ctrl == "listview" || lo_ctrl == "gridview" || lo_ctrl == "flipview" || lo_ctrl == "combobox" || lo_ctrl == "pivot")
                    coll_prop = "Items";
                for (auto const& cv : child_vars)
                    out << indent << "inspectable_append(" << var_name << "." << coll_prop << "(), " << cv << ");\n";
            }
            else if (ck == "items_panel")
            {
                std::string coll_prop = "Items";
                std::string lo_ctrl(n.control);
                for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lo_ctrl == "commandbar") coll_prop = "SecondaryCommands";
                else if (lo_ctrl == "commandbarflyout") coll_prop = "SecondaryCommands";
                else if (lo_ctrl == "navigationviewitem") coll_prop = "MenuItems";
                for (auto const& cv : child_vars)
                    out << indent << "inspectable_append(" << var_name << "." << coll_prop << "(), " << cv << ");\n";
            }
            else if (ck == "keyboard")
            {
                // KeyboardAccelerator → .KeyboardAccelerators() via inspectable_append
                for (auto const& cv : child_vars)
                    out << indent << "inspectable_append(" << var_name << ".KeyboardAccelerators(), " << cv << ");\n";
            }
            else if (ck == "icon_source")
            {
                // ImageIconSource → .IconSource(child); setter takes IconSource (Media)
                for (auto const& cv : child_vars)
                    out << indent << var_name << ".IconSource(winrt::Microsoft::UI::Xaml::Controls::IconSource::wrap_borrowed(" << cv << ".m_unknown.get()));\n";
            }
            else if (ck == "navigation_item")
            {
                // NavigationViewItem: icon children → .Icon(), rest → .MenuItems().Append()
                for (size_t i = 0; i < n.children.size() && i < child_vars.size(); ++i)
                {
                    if (is_icon_element(n.children[i].control))
                        out << indent << var_name << ".Icon(IconElement::wrap_borrowed(" << child_vars[i] << ".m_unknown.get()));\n";
                    else
                        out << indent << "inspectable_append(" << var_name << ".MenuItems(), " << child_vars[i] << ");\n";
                }
            }
            else if (ck == "navigation")
            {
                // NavigationView: first child is Frame/Content, rest are MenuItems
                bool first = true;
                for (size_t i = 0; i < n.children.size() && i < child_vars.size(); ++i)
                {
                    std::string child_ctrl = n.children[i].control;
                    if ((child_ctrl == "Frame" || child_ctrl == "Grid") && first)
                    {
                        out << indent << var_name << ".Content(" << child_vars[i] << ");\n";
                        first = false;
                    }
                    else
                    {
                        out << indent << "inspectable_append(" << var_name << ".MenuItems(), " << child_vars[i] << ");\n";
                    }
                }
            }
            else if (ck == "documents")
            {
                // Documents types (Paragraph, Span, etc.)
                // Children are Inline elements, route via Inlines() using inspectable_append
                for (auto const& cv : child_vars)
                    out << indent << "inspectable_append(" << var_name << ".Inlines(), " << cv << ");\n";
            }
            else if (ck == "blocks")
            {
                // RichTextBlock: children go into Blocks() using inspectable_append
                for (auto const& cv : child_vars)
                    out << indent << "inspectable_append(" << var_name << ".Blocks(), " << cv << ");\n";
            }
            else if (ck == "content")
            {
                std::string scp = catalog.single_child_prop(n.control);
                if (scp.empty())
                {
                    // Control has no child/content property — skip children
                }
                else if (has_icon_child && control_accepts_icon_prop(n.control))
                {
                    for (size_t i = 0; i < n.children.size() && i < child_vars.size(); ++i)
                    {
                        if (is_icon_element(n.children[i].control))
                            out << indent << var_name << ".Icon(IconElement::wrap_borrowed(" << child_vars[i] << ".m_unknown.get()));\n";
                    }
                    std::vector<std::string> non_icon;
                    for (size_t i = 0; i < n.children.size() && i < child_vars.size(); ++i)
                        if (!is_icon_element(n.children[i].control))
                            non_icon.push_back(child_vars[i]);
                    if (non_icon.size() == 1)
                        out << indent << var_name << "." << scp << "(" << catalog.cast_child(non_icon[0], n.control) << ");\n";
                    else if (non_icon.size() > 1)
                    {
                        out << indent << "{\n";
                        out << indent << "    auto _wrap_sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel();\n";
                        for (auto const& cv : non_icon)
                            out << indent << "    _wrap_sp.Children().Append(" << cv << ");\n";
                        out << indent << "    " << var_name << "." << scp << "(_wrap_sp);\n";
                        out << indent << "}\n";
                    }
                }
                else
                {
                    if (scp.empty())
                    {
                        // Control has no child/content property — skip children
                    }
                    else if (scp == "Children")
                    {
                        // Panel controls: append children directly, no wrapping needed
                        for (auto const& cv : child_vars)
                            out << indent << var_name << ".Children().Append(" << cv << ");\n";
                    }
                    else if (scp == "Items" || scp == "SwipeItems" || scp == "MenuItems" || scp == "Inlines" || scp == "Blocks")
                    {
                        // Collection controls: use inspectable_append
                        for (auto const& cv : child_vars)
                            out << indent << "inspectable_append(" << var_name << "." << scp << "(), " << cv << ");\n";
                    }
                    else if (n.control == "SwitchPresenter")
                    {
                        if (!child_vars.empty())
                        {
                            out << indent << var_name << ".Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << child_vars[0] << ".m_unknown.get()));\n";
                            if (named_elements.count("FilterBar") && child_vars.size() >= 2)
                            {
                                out << indent << "FilterBar.SelectionChanged([this, " << var_name << ", p0 = " << child_vars[0] << ", p1 = " << child_vars[1] << "](auto const&, auto const&)\n";
                                out << indent << "{\n";
                                out << indent << "    auto sel = FilterBar.SelectedItem();\n";
                                out << indent << "    if (sel.get_abi())\n";
                                out << indent << "    {\n";
                                out << indent << "        auto tag = winrt::unbox_value_or<winrt::hstring>(sel.Tag(), winrt::hstring{});\n";
                                out << indent << "        if (tag == L\"Favorites\") {\n";
                                out << indent << "            " << var_name << ".Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(p1.m_unknown.get()));\n";
                                out << indent << "            if (FavoriteSamples.m_unknown.get()) FavoriteSamples.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n";
                                out << indent << "            if (FavoriteSamplesFallbackMessage.m_unknown.get()) FavoriteSamplesFallbackMessage.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);\n";
                                out << indent << "        } else {\n";
                                out << indent << "            " << var_name << ".Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(p0.m_unknown.get()));\n";
                                out << indent << "        }\n";
                                out << indent << "        try {\n";
                                out << indent << "            auto items = FilterBar.get_Items();\n";
                                out << indent << "            if (items && items.get_Size() >= 2) {\n";
                                out << indent << "                auto it0 = items.GetAt(0);\n";
                                out << indent << "                auto it1 = items.GetAt(1);\n";
                                out << indent << "                auto accent_b = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{255, 118, 185, 237});\n";
                                out << indent << "                auto dark_b = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{255, 0, 0, 0});\n";
                                out << indent << "                auto gray_b = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{255, 53, 53, 53});\n";
                                out << indent << "                auto white_b = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255});\n";
                                out << indent << "                auto border_gray = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{255, 64, 64, 64});\n";
                                out << indent << "                bool fav = (tag == L\"Favorites\");\n";
                                out << indent << "                it0.Background(fav ? gray_b : accent_b);\n";
                                out << indent << "                it0.Foreground(fav ? white_b : dark_b);\n";
                                out << indent << "                it0.BorderBrush(fav ? border_gray : accent_b);\n";
                                out << indent << "                auto ic0 = it0.get_Icon();\n";
                                out << indent << "                if (ic0.m_unknown.get()) ic0.Foreground(fav ? white_b : dark_b);\n";
                                out << indent << "                it1.Background(fav ? accent_b : gray_b);\n";
                                out << indent << "                it1.Foreground(fav ? dark_b : white_b);\n";
                                out << indent << "                it1.BorderBrush(fav ? accent_b : border_gray);\n";
                                out << indent << "                auto ic1 = it1.get_Icon();\n";
                                out << indent << "                if (ic1.m_unknown.get()) ic1.Foreground(fav ? dark_b : white_b);\n";
                                out << indent << "            }\n";
                                out << indent << "        } catch (...) {}\n";
                                out << indent << "    }\n";
                                out << indent << "});\n";
                            }
                        }
                    }
                    else if (child_vars.size() == 1)
                        out << indent << var_name << "." << scp << "(" << catalog.cast_child(child_vars[0], n.control) << ");\n";
                    else if (n.control == "SettingsExpander")
                    {
                        for (size_t i = 0; i < child_vars.size(); ++i)
                        {
                            if (i == 0 && (n.children[i].control == "ToggleSwitch" || n.children[i].control == "Button" || n.children[i].control == "ComboBox"))
                                out << indent << var_name << ".Content(" << catalog.cast_child(child_vars[i], n.control) << ");\n";
                            else
                                out << indent << var_name << ".Items(" << catalog.cast_child(child_vars[i], n.control) << ");\n";
                        }
                    }
                    else
                    {
                        {
                            out << indent << "{\n";
                            out << indent << "    auto _wrap_sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel();\n";
                            for (auto const& cv : child_vars)
                                out << indent << "    _wrap_sp.Children().Append(" << cv << ");\n";
                            out << indent << "    " << var_name << "." << scp << "(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(_wrap_sp.m_unknown.get()));\n";
                            out << indent << "}\n";
                        }
                    }
                }
            }
            else if (ck == "popup")
            {
                // Popup uses .Child() for single child
                if (child_vars.size() == 1)
                    out << indent << var_name << ".Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << child_vars[0] << ".m_unknown.get()));\n";
                else if (!child_vars.empty())
                {
                    // Multiple children → wrap in StackPanel
                    out << indent << "{\n";
                    out << indent << "    auto _wrap_sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel();\n";
                    for (auto const& cv : child_vars)
                        out << indent << "    _wrap_sp.Children().Append(" << cv << ");\n";
                    out << indent << "    " << var_name << ".Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(_wrap_sp.m_unknown.get()));\n";
                    out << indent << "}\n";
                }
            }
            else if (ck == "columns")
            {
                // TableView: IVector<TableViewColumn> columns collection
                for (auto const& cv : child_vars)
                    out << indent << "inspectable_append(" << var_name << ".Columns(), " << cv << ");\n";
            }
            else if (ck == "charts")
            {
                // Chart children (the .winui intermediate flattens the
                // Chart.Data / Chart.Axes property elements): route by the
                // child's own type — Samples -> Data(), Axis -> Axes(),
                // CartesianSeries -> Series().
                for (size_t i = 0; i < child_vars.size(); ++i)
                {
                    std::string lc;
                    if (child_ctrls.size() == child_vars.size()) lc = child_ctrls[i];
                    else if (i < n.children.size()) lc = n.children[i].control;
                    for (auto& c : lc) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    auto scq = lc.rfind("::");
                    if (scq != std::string::npos) lc = lc.substr(scq + 2);
                    if (lc.size() >= 7 && lc.compare(lc.size() - 7, 7, "samples") == 0)
                        out << indent << "inspectable_append(" << var_name << ".Data(), " << child_vars[i] << ");\n";
                    else if (lc.size() >= 4 && lc.compare(lc.size() - 4, 4, "axis") == 0)
                        out << indent << "inspectable_append(" << var_name << ".Axes(), " << child_vars[i] << ");\n";
                    else if (lc.size() >= 6 && lc.compare(lc.size() - 6, 6, "series") == 0)
                        out << indent << "inspectable_append(" << var_name << ".Series(), " << child_vars[i] << ");\n";
                    else
                        out << indent << "// " << var_name << " child skipped: unknown Charts collection (" << lc << ")\n";
                }
            }
            else if (ck == "panel")
            {
                // Panel controls always use .Children().Append() — even if they are component types
                // (Column, Row are StackPanel aliases that resolve to platform StackPanel)
                bool is_known_panel = (n.control == "StackPanel" || n.control == "Grid" || n.control == "Canvas"
                    || n.control == "RelativePanel" || n.control == "WrapPanel"
                    || n.control == "Column" || n.control == "Row");
                if (is_known_panel)
                {
                    for (auto const& cv : child_vars)
                        out << indent << var_name << ".Children().Append(" << cv << ");\n";
                }
                else if (is_app_or_unknown_type(n.control))
                {
                    // Unknown component types don't have Children() - wrap in StackPanel and set Content
                    std::string scp = catalog.single_child_prop(n.control);
                    if (scp.empty())
                    {
                        // No content property: attach children to the nearest
                        // enclosing panel so conditional subtrees still render.
                        if (!parent_chain.empty())
                        {
                            for (auto const& cv : child_vars)
                                out << indent << parent_chain.back() << ".Children().Append(" << cv << ");\n";
                        }
                    }
                    else if (n.control == "ControlExample")
                    {
                        std::vector<std::string> ex_vars, out_vars, opt_vars;
                        for (size_t i = 0; i < child_vars.size(); ++i)
                        {
                            std::string sl = (i < child_slots.size()) ? child_slots[i] : "";
                            if (sl == "Output") out_vars.push_back(child_vars[i]);
                            else if (sl == "Options") opt_vars.push_back(child_vars[i]);
                            else ex_vars.push_back(child_vars[i]);
                        }
                        auto emit_slot = [&](std::string const& slot_prop, std::vector<std::string> const& svars)
                        {
                            if (svars.size() == 1)
                            {
                                out << indent << var_name << "." << slot_prop << "(" << svars[0] << ");\n";
                            }
                            else if (svars.size() > 1)
                            {
                                out << indent << "{\n";
                                out << indent << "    auto _wrap_sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel();\n";
                                for (auto const& cv : svars)
                                    out << indent << "    _wrap_sp.Children().Append(" << cv << ");\n";
                                out << indent << "    " << var_name << "." << slot_prop << "(_wrap_sp);\n";
                                out << indent << "}\n";
                            }
                        };
                        emit_slot("Example", ex_vars);
                        emit_slot("Output", out_vars);
                        emit_slot("Options", opt_vars);
                    }
                    else
                    {
                        std::vector<std::string> items_vars;
                        std::vector<std::string> content_vars;
                        for (size_t i = 0; i < n.children.size() && i < child_vars.size(); ++i)
                        {
                            std::string sl = (i < child_slots.size()) ? child_slots[i] : "";
                            if (sl == "Items") items_vars.push_back(child_vars[i]);
                            else content_vars.push_back(child_vars[i]);
                        }
                        if (!items_vars.empty())
                        {
                            for (auto const& iv : items_vars)
                                out << indent << "inspectable_append(" << var_name << ".Items(), " << iv << ");\n";
                            if (content_vars.size() == 1)
                            {
                                out << indent << var_name << "." << scp << "(" << content_vars[0] << ");\n";
                            }
                            else if (content_vars.size() > 1)
                            {
                                out << indent << "{\n";
                                out << indent << "    auto _wrap_sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel();\n";
                                for (auto const& cv : content_vars)
                                    out << indent << "    _wrap_sp.Children().Append(" << cv << ");\n";
                                out << indent << "    " << var_name << "." << scp << "(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(_wrap_sp.m_unknown.get()));\n";
                                out << indent << "}\n";
                            }
                        }
                        else
                        {
                            if (child_vars.size() == 1)
                            {
                                out << indent << var_name << "." << scp << "(" << child_vars[0] << ");\n";
                            }
                            else
                            {
                                out << indent << "{\n";
                                out << indent << "    auto _wrap_sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel();\n";
                                for (auto const& cv : child_vars)
                                    out << indent << "    _wrap_sp.Children().Append(" << cv << ");\n";
                                out << indent << "    " << var_name << "." << scp << "(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(_wrap_sp.m_unknown.get()));\n";
                                out << indent << "}\n";
                            }
                        }
                    }
                }
                else
                    for (auto const& cv : child_vars)
                        out << indent << "inspectable_append(" << var_name << ".Children(), " << cv << ");\n";
            }
            else
            {
                std::string scp = catalog.single_child_prop(n.control);
                // Fallback: route icon children to .Icon(), rest to .Content/.Children
                if (has_icon_child && control_accepts_icon_prop(n.control))
                {
                    for (size_t i = 0; i < n.children.size() && i < child_vars.size(); ++i)
                    {
                        if (is_icon_element(n.children[i].control))
                            out << indent << var_name << ".Icon(IconElement::wrap_borrowed(" << child_vars[i] << ".m_unknown.get()));\n";
                    }
                    std::vector<std::string> non_icon;
                    for (size_t i = 0; i < n.children.size() && i < child_vars.size(); ++i)
                        if (!is_icon_element(n.children[i].control))
                            non_icon.push_back(child_vars[i]);
                    if (non_icon.size() == 1)
                        out << indent << var_name << "." << scp << "(" << catalog.cast_child(non_icon[0], n.control) << ");\n";
                    else if (non_icon.size() > 1)
                    {
                        out << indent << "{\n";
                        out << indent << "    auto _wrap_sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel();\n";
                        for (auto const& cv : non_icon)
                            out << indent << "    _wrap_sp.Children().Append(" << cv << ");\n";
                        out << indent << "    " << var_name << "." << scp << "(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(_wrap_sp.m_unknown.get()));\n";
                        out << indent << "}\n";
                    }
                }
                else
                {
                    if (scp.empty())
                    {
                        // Control has no child/content property — skip children
                    }
                    else if (child_vars.size() == 1)
                        out << indent << var_name << "." << scp << "(" << catalog.cast_child(child_vars[0], n.control) << ");\n";
                    else if (n.control == "SettingsExpander")
                    {
                        for (size_t i = 0; i < child_vars.size(); ++i)
                        {
                            if (i == 0 && n.children[i].control == "ToggleSwitch")
                                out << indent << var_name << ".Content(" << catalog.cast_child(child_vars[i], n.control) << ");\n";
                            else
                                out << indent << var_name << ".Items(" << catalog.cast_child(child_vars[i], n.control) << ");\n";
                        }
                    }
                    else
                    {
                        // Always wrap multiple children in StackPanel for content controls
                        out << indent << "{\n";
                        out << indent << "    auto _wrap_sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel();\n";
                        for (auto const& cv : child_vars)
                            out << indent << "    _wrap_sp.Children().Append(" << cv << ");\n";
                        out << indent << "    " << var_name << "." << scp << "(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(_wrap_sp.m_unknown.get()));\n";
                        out << indent << "}\n";
                    }
                }
            }
        }



        return var_name;
    };

