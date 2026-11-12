// winuic_template.ixx — DataTemplate projection and runtime helper code generators
// Part of module winuic_data
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

// DataTemplate projection helpers.
// Since DataTemplate is a sealed runtime class with no code-only factory
// (even real C++/WinRT v3 lacks DataTemplate::From), the only path to create
// one programmatically is XamlReader::Load(xamlString).

// Check if a raw attr value looks like a template JSON payload.
inline auto is_template_payload(std::string const& key, std::string const& val) -> bool
{
    auto lk = key;
    for (auto& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lk.find("template") != std::string::npos
        && !val.empty()
        && val.front() == '{'
        && val.find("itemType") != std::string::npos
        && val.find("body") != std::string::npos;
}

// Escape a string for embedding in a C++ wide-string literal L"...".
inline auto escape_for_wstring(std::string const& s) -> std::string
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
    {
        if (c == 0x5C) { out += (char)0x5C; out += (char)0x5C; }
        else if (c == 0x22) { out += (char)0x5C; out += (char)0x22; }
        else out += (char)c;
    }
    return out;
}

// Minimal recursive-descent JSON parser for template payloads only.
struct template_json_parser
{
    std::string_view s;
    std::size_t pos = 0;

    void skip_ws() { while (pos < s.size() && (s[pos]==' '||s[pos]=='\t'||s[pos]=='\n'||s[pos]=='\r')) ++pos; }
    char advance() { return pos < s.size() ? s[pos++] : '\0'; }

    std::string parse_string()
    {
        skip_ws();
        if (pos >= s.size() || s[pos] != '"') return {};
        ++pos;
        std::string out;
        while (pos < s.size())
        {
            if (s[pos] == '\\')
            {
                // Count the whole backslash run. An EVEN run means the quote is
                // not escaped (each pair is a literal backslash); an ODD run means
                // the following char is escaped. Handling the run as a unit stops
                // template payloads with run-escaped quotes (\\" from winuic's
                // JSON escaping) from terminating the string early and spinning.
                std::size_t run = 0;
                while (pos + run < s.size() && s[pos + run] == '\\') ++run;
                if (run % 2 == 0)
                {
                    // Even run: `\\` is a literal backslash pair. If a quote
                    // follows, winuic's template payloads double-escaped it
                    // (`\\"` for a literal quote inside a DataTemplate).
                    // Consume the quote as a literal char rather than ending
                    // the string, so the value parses as one continuous string.
                    out.append(run / 2, '\\');
                    pos += run;
                    if (pos < s.size() && s[pos] == '"') { out += '"'; ++pos; continue; }
                    continue;
                }
                out.append(run / 2, '\\');
                pos += run;
                if (pos >= s.size()) break;
                switch (s[pos]) { case '"': out += '"'; break; case '\\': out += '\\'; break; case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break; default: out += s[pos]; break; }
                ++pos;
                continue;
            }
            if (s[pos] == '"') { ++pos; break; }
            out += s[pos];
            ++pos;
        }
        return out;
    }

    void skip_value()
    {
        skip_ws();
        if (pos >= s.size()) return;
        char c = s[pos];
        if (c == '"') { parse_string(); return; }
        if (c == '{' || c == '[') { char close = (c == '{' ? '}' : ']'); advance(); int d = 1; while (pos < s.size() && d > 0) { if (s[pos] == c) ++d; else if (s[pos] == close) --d; else if (s[pos] == '"') { parse_string(); continue; } ++pos; } return; }
        while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']') ++pos;
    }

    std::string parse_member_value()
    {
        skip_ws();
        if (pos < s.size() && s[pos] == ':') ++pos;
        skip_ws();
        if (pos < s.size() && s[pos] == '"') return parse_string();
        std::string out;
        while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']') out += s[pos++];
        return out;
    }

    std::string extract_string(std::string_view field)
    {
        skip_ws();
        if (pos >= s.size() || s[pos] != '{') return {};
        advance();
        while (pos < s.size() && s[pos] != '}')
        {
            std::string key = parse_string();
            if (key == field) return parse_member_value();
            skip_ws();
            if (pos < s.size() && s[pos] == ':') ++pos;
            skip_value();
            if (pos < s.size() && s[pos] == ',') ++pos;
        }
        return {};
    }

    template <typename Fn>
    void for_each_array_element(std::string_view field, Fn&& fn)
    {
        skip_ws();
        if (pos >= s.size() || s[pos] != '{') return;
        advance();
        while (pos < s.size() && s[pos] != '}')
        {
            std::string key = parse_string();
            if (key == field)
            {
                skip_ws(); if (pos < s.size() && s[pos] == ':') ++pos;
                skip_ws(); if (pos >= s.size() || s[pos] != '[') return; advance();
                while (pos < s.size() && s[pos] != ']')
                {
                    skip_ws(); if (s[pos] == ']') break;
                    std::size_t start = pos; skip_value(); std::size_t end = pos;
                    fn(s.substr(start, end - start));
                    if (pos < s.size() && s[pos] == ',') ++pos;
                }
                return;
            }
            skip_ws(); if (pos < s.size() && s[pos] == ':') ++pos;
            skip_value(); if (pos < s.size() && s[pos] == ',') ++pos;
        }
    }
};

// Convert a template body JSON node into a XAML element string.
inline auto json_node_to_xaml(std::string_view json) -> std::string
{
    template_json_parser p; p.s = json; p.pos = 0;
    std::string ctrl = p.extract_string("control");
    if (ctrl.empty()) return {};
    std::string xaml = "<" + ctrl;
    // props
    { template_json_parser pp; pp.s = json; pp.pos = 0;
      pp.skip_ws(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == '{') pp.advance();
      while (pp.pos < pp.s.size() && pp.s[pp.pos] != '}') {
          std::string key = pp.parse_string();
          if (key == "props") {
              pp.skip_ws(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == ':') pp.advance();
              pp.skip_ws(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == '{') {
                  pp.advance();
                  while (pp.pos < pp.s.size() && pp.s[pp.pos] != '}') {
                      std::string pk = pp.parse_string(); std::string pv = pp.parse_member_value();
                      if (pv.size() >= 2 && pv.front() == '"' && pv.back() == '"') pv = pv.substr(1, pv.size() - 2);
                      if (!pv.empty() && pv.front() == '$') {
                          std::string b = pv.substr(1); auto d = b.find('.'); if (d != std::string::npos) b = b.substr(d + 1);
                          if (!b.empty()) b[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(b[0])));
                          xaml += " " + pk + "=\"{x:Bind " + b + "}\"";
                      } else {
                          std::string esc; for (char ch : pv) { switch (ch) { case '&': esc += "&amp;"; break; case '<': esc += "&lt;"; break; case '>': esc += "&gt;"; break; case '"': esc += "&quot;"; break; default: esc += ch; } }
                          xaml += " " + pk + "=\"" + esc + "\"";
                      }
                      if (pp.pos < pp.s.size() && pp.s[pp.pos] == ',') pp.advance();
                  }
              } break;
          }
          pp.skip_ws(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == ':') pp.advance();
          pp.skip_value(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == ',') pp.advance();
      }
    }
    xaml += ">";
    // children
    { template_json_parser pc; pc.s = json; pc.pos = 0;
      pc.skip_ws(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == '{') pc.advance();
      while (pc.pos < pc.s.size() && pc.s[pc.pos] != '}') {
          std::string key = pc.parse_string();
          if (key == "children") {
              pc.skip_ws(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == ':') pc.advance();
              pc.skip_ws(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == '[') {
                  pc.advance();
                  while (pc.pos < pc.s.size() && pc.s[pc.pos] != ']') {
                      pc.skip_ws(); if (pc.s[pc.pos] == ']') break;
                      std::size_t start = pc.pos; pc.skip_value(); std::size_t end = pc.pos;
                      std::string cx = json_node_to_xaml(pc.s.substr(start, end - start));
                      if (!cx.empty()) xaml += cx;
                      if (pc.pos < pc.s.size() && pc.s[pc.pos] == ',') pc.advance();
                  }
              } break;
          }
          pc.skip_ws(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == ':') pc.advance();
          pc.skip_value(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == ',') pc.advance();
      }
    }
    xaml += "</" + ctrl + ">";
    return xaml;
}

// Convert a full template JSON payload to a XAML DataTemplate string.
inline auto template_payload_to_xaml(std::string_view payload) -> std::string
{
    template_json_parser p; p.s = payload; p.pos = 0;
    std::string body;
    p.for_each_array_element("body", [&](std::string_view elem) {
        std::string child = json_node_to_xaml(elem);
        if (!child.empty()) body += child;
    });
    return "<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\""
           " xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">"
           + body + "</DataTemplate>";
}


// DataTemplate factory function generator (React-style: no load_xaml).
// Converts template JSON payloads into C++ factory functions that build
// the visual tree using pure WinUI API calls at compile time.

struct template_factory_result {
    std::string func_name;
    std::string func_body;
    std::string item_type;
};

// Recursively convert a template JSON node to C++ control tree code.
// item_kind: "hstring"/"int32"/"double" when the template's __item was
// unboxed to a concrete type, "" when it stays an IInspectable.
inline auto json_node_to_cpp(
    std::string_view json, std::ostringstream& out,
    std::string const& indent, int& var_counter,
    std::string const& item_var, attribute_catalog const& catalog,
    std::string const& item_kind = {}
) -> std::string
{
    template_json_parser p; p.s = json; p.pos = 0;
    std::string ctrl = p.extract_string("control");
    if (ctrl.empty()) return {};

    // Skip dotted names (e.g. "Grid.ContextFlyout") - attached properties, not types
    if (ctrl.find('.') != std::string::npos) return {};

    std::string winui_type = catalog.winui3_type(ctrl);
    std::string var_name = "__" + sanitize_id(ctrl) + "_" + std::to_string(var_counter++);

    out << indent << "auto " << var_name << " = " << winui_type << "();\n";

    // Parse props
    { template_json_parser pp; pp.s = json; pp.pos = 0;
      pp.skip_ws(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == '{') pp.advance();
      while (pp.pos < pp.s.size() && pp.s[pp.pos] != '}') {
          std::string key = pp.parse_string();
          if (key == "props") {
              pp.skip_ws(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == ':') pp.advance();
              pp.skip_ws(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == '{') {
                  pp.advance();
                  while (pp.pos < pp.s.size() && pp.s[pp.pos] != '}') {
                      std::string pk = pp.parse_string();
                      std::string tpl_prop = ctrl_prop(ctrl, pk);
                      std::string pv = pp.parse_member_value();
                      // Bare {x:Bind}/{Binding} means "the item itself". When the
                      // item was unboxed to a concrete hstring (x:String templates)
                      // and the prop is Text, emit a direct data binding instead of
                      // dropping the value: __TextBlock.Text(__item).
                      {
                          std::string pv_bind = pv;
                          if (pv_bind.size() >= 2 && pv_bind.front() == '\"' && pv_bind.back() == '\"')
                              pv_bind = pv_bind.substr(1, pv_bind.size() - 2);
                          while (!pv_bind.empty() && (pv_bind.back() == ' ' || pv_bind.back() == '\t')) pv_bind.pop_back();
                          if (!pk.empty() && (pv_bind == "{x:Bind}" || pv_bind == "{Binding}" || pv_bind == "{binding}"))
                          {
                              std::string pk_lo_b = pk;
                              for (auto& c : pk_lo_b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                              if (item_kind == "hstring" && pk_lo_b == "text")
                              {
                                  out << indent << var_name << "." << tpl_prop << "(" << item_var << ");\n";
                                  if (pp.pos < pp.s.size() && pp.s[pp.pos] == ',') pp.advance();
                                  continue;
                              }
                          }
                      }
                      // Skip event handler props (contain {, }, (, ) or start with 'on')
                      bool is_event = false;
                      if (!pk.empty() && pk.size() >= 2 && pk[0] == 'o' && pk[1] == 'n') is_event = true;
                      if (pk.find('{') != std::string::npos || pk.find('}') != std::string::npos
                          || pk.find('(') != std::string::npos || pk.find(')') != std::string::npos
                          || pv.find('{') != std::string::npos || pv.find('}') != std::string::npos
                          || pv.find('(') != std::string::npos)
                          is_event = true;
                      if (is_event || pk.empty()) { if (pp.pos < pp.s.size() && pp.s[pp.pos] == ',') pp.advance(); continue; }
                      if (pv.size() >= 2 && pv.front() == '"' && pv.back() == '"')
                          pv = pv.substr(1, pv.size() - 2);

                      // item.Property -> the xaml2winuic member accessor. Member
                      // "root" on a value-typed item (x:String/Int32/Double) means
                      // "the item itself": Text="{x:Bind}" arrives as item.root.
                      if (pv.size() > 5 && pv.substr(0, 5) == "item.") {
                          std::string member_orig = pv.substr(5); // keep original case ("root")
                          std::string prop = member_orig;
                          if (!prop.empty()) prop[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(prop[0])));
                          std::string pk_lo_item = pk;
                          for (auto& c : pk_lo_item) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          // Concrete hstring item + "root" member + Text prop: bind the item text.
                          if (item_kind == "hstring" && member_orig == "root" && pk_lo_item == "text")
                          {
                              out << indent << var_name << "." << tpl_prop
                                  << "(" << item_var << ");\n";
                          }
                          else if (item_kind == "obj" && member_orig == "root")
                          {
                              // The item itself is a projected platform object (e.g.
                              // SolidColorBrush) - bind it by casting __item to the
                              // property's expected base type.
                              bool brush_prop = (pk_lo_item == "background" || pk_lo_item == "foreground"
                                  || pk_lo_item == "fill" || pk_lo_item == "stroke"
                                  || pk_lo_item == "borderbrush" || pk_lo_item == "opacitymask");
                              if (brush_prop)
                                  out << indent << var_name << "." << tpl_prop
                                      << "(winrt::Microsoft::UI::Xaml::Media::Brush::wrap_borrowed(" << item_var << ".get_abi()));\n";
                              else if (pk_lo_item == "content" || pk_lo_item == "header" || pk_lo_item == "tag")
                                  out << indent << var_name << "." << tpl_prop << "(" << item_var << ");\n";
                              // else: member access on an unprojected model - no cast possible
                          }
                          else if (item_var != "__item")
                          {
                              out << indent << var_name << "." << prop
                                  << "(" << item_var << "." << prop << "());\n";
                          }
                          // else: IInspectable/untyped or unsupported scalar item — skip
                      }
                      else if (!pv.empty() && pv[0] == '$') {
                          std::string b = pv.substr(1);
                          auto d = b.find('.');
                          if (d != std::string::npos) b = b.substr(d + 1);
                          if (!b.empty()) b[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(b[0])));
                          if (item_var == "__item") {
                              out << indent << var_name << "." << tpl_prop
                                  << "(winrt::unbox_value<winrt::hstring>(" << item_var << "));\n";
                          } else {
                              out << indent << var_name << "." << tpl_prop
                                  << "(self->" << b << ");\n";
                          }
                      }
                      else if (!pv.empty() && (std::isdigit(static_cast<unsigned char>(pv.front())) || pv.front() == '-')) {
                          std::string pk_lo_cr = pk;
                          for (auto& c : pk_lo_cr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          if (pk_lo_cr == "cornerradius")
                              out << indent << var_name << "." << tpl_prop
                                  << "(winrt::Microsoft::UI::Xaml::CornerRadius{" << pv << "," << pv << "," << pv << "," << pv << "});\n";
                          else if (pk_lo_cr == "padding" || pk_lo_cr == "margin" || pk_lo_cr == "borderthickness")
                          {
                              std::string rv_t = resolve_prop_value(pk, pv);
                              if (!rv_t.empty())
                                  out << indent << var_name << "." << tpl_prop << "(" << rv_t << ");\n";
                              else
                                  out << indent << var_name << "." << tpl_prop
                                      << "(winrt::Microsoft::UI::Xaml::Thickness{" << pv << "," << pv << "," << pv << "," << pv << "});\n";
                          }
                          else
                              out << indent << var_name << "." << tpl_prop
                                  << "(" << pv << ");\n";
                      }
                      else if (pv == "true" || pv == "false" || pv == "nullptr") {
                          std::string pk_lo_b2 = pk;
                          for (auto& c : pk_lo_b2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          if (pk_lo_b2 == "ischecked")
                              out << indent << var_name << "." << tpl_prop
                                  << "(winrt::Windows::Foundation::IReference<bool>(winrt::box_value(" << pv << ")));\n";
                          else if (pk_lo_b2 == "oncontent" || pk_lo_b2 == "offcontent" || pk_lo_b2 == "content" || pk_lo_b2 == "header" || pk_lo_b2 == "tag")
                              out << indent << var_name << "." << tpl_prop
                                  << "(winrt::box_value(" << pv << "));\n";
                          else if (pk_lo_b2 == "bold" || pk_lo_b2 == "fontweight")
                              out << indent << var_name << "." << tpl_prop
                                  << "(winrt::Windows::UI::Text::FontWeight{" << (pv == "true" ? "700" : "400") << "});\n";
                          else if (pk_lo_b2 == "italic" || pk_lo_b2 == "fontstyle")
                              out << indent << var_name << "." << tpl_prop
                                  << "(winrt::Windows::UI::Text::FontStyle::" << (pv == "true" ? "Italic" : "Normal") << ");\n";
                          else
                              out << indent << var_name << "." << tpl_prop
                                  << "(" << pv << ");\n";
                      }
                      else if (!pv.empty()) {
                          std::string collapsed = normalize_payload_value(pv);
                          for (char c : collapsed) { if (c == '\n' || c == '\r') { c = ' '; } }
                          // Skip empty, quote-only, or backslash-only values (template binding gaps)
                          if (collapsed.empty() || collapsed == "\"" || collapsed == "\\") {
                              if (pp.pos < pp.s.size() && pp.s[pp.pos] == ',') pp.advance();
                              continue;
                          }
                          std::string pk_lo = pk;
                          for (auto& c : pk_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          bool is_grid_dim = (pk_lo == "width" || pk_lo == "height");
                          // Handle star sizes: "1*" -> winrt::Microsoft::UI::Xaml::GridLength(1, GridUnitType::Star)
                          if (is_grid_dim && !collapsed.empty() && collapsed.back() == '*') {
                              std::string num = collapsed.substr(0, collapsed.size() - 1);
                              if (num.empty() || num == "\"") num = "1";
                              out << indent << var_name << "." << tpl_prop
                                  << "(winrt::Microsoft::UI::Xaml::GridLength(" << num << ", winrt::Microsoft::UI::Xaml::GridUnitType::Star));\n";
                          }
                          else if (is_grid_dim && collapsed == "Auto") {
                              out << indent << var_name << "." << tpl_prop
                                  << "(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));\n";
                          }
                          // Handle bool values (with Visibility property-aware check)
                          else if (collapsed == "true" || collapsed == "false" || collapsed == "True" || collapsed == "False") {
                              std::string bv = (collapsed == "True" || collapsed == "true") ? "true" : "false";
                              std::string pk_lo2 = pk;
                              for (auto& c : pk_lo2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                              if (pk_lo2 == "visible" || pk_lo2 == "visibility") {
                                  std::string vis_val = (bv == "true") ? "winrt::Microsoft::UI::Xaml::Visibility::Visible" : "winrt::Microsoft::UI::Xaml::Visibility::Collapsed";
                                  out << indent << var_name << "." << tpl_prop << "(" << vis_val << ");\n";
                              } else if (pk_lo2 == "disabled") {
                                  std::string inv = (bv == "true") ? "false" : "true";
                                  out << indent << var_name << "." << tpl_prop << "(" << inv << ");\n";
                              } else if (pk_lo2 == "icon" || pk_lo2 == "iconsource" || pk_lo2 == "symbol") {
                                  // Skip bool values for icon/symbol props (not valid enum names)
                              } else {
                                  out << indent << var_name << "." << tpl_prop << "(" << bv << ");\n";
                              }
                          }
                          // Handle numeric values
                          else if (!collapsed.empty() && (std::isdigit(static_cast<unsigned char>(collapsed.front())) || collapsed.front() == '-')) {
                              // Thickness/CornerRadius need struct wrapping
                              std::string pk_lo3 = pk;
                              for (auto& c : pk_lo3) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                              if (pk_lo3 == "cornerradius")
                                  out << indent << var_name << "." << tpl_prop << "(winrt::Microsoft::UI::Xaml::CornerRadius{" << collapsed << "," << collapsed << "," << collapsed << "," << collapsed << "});\n";
                              else if (pk_lo3 == "padding" || pk_lo3 == "margin" || pk_lo3 == "borderthickness")
                              {
                                  // Multi-value ("0,12,0,12") and single-value both need
                                  // correct Thickness expansion.
                                  std::string rv3 = resolve_prop_value(pk, collapsed);
                                  if (!rv3.empty())
                                      out << indent << var_name << "." << tpl_prop << "(" << rv3 << ");\n";
                                  else
                                      out << indent << var_name << "." << tpl_prop << "(winrt::Microsoft::UI::Xaml::Thickness{" << collapsed << "," << collapsed << "," << collapsed << "," << collapsed << "});\n";
                              }
                              else
                                  out << indent << var_name << "." << tpl_prop << "(" << collapsed << ");\n";
                          }
                          else {
                              // Enum or other string value
                              std::string resolved = resolve_prop_value(pk, collapsed);
                              if (!resolved.empty()) {
                                  // Let box_if_inspectable decide wrapping — blanket IInspectable() wrapping
                                  // breaks typed setters (FontWeight, Uri, enums).
                                  out << indent << var_name << "." << tpl_prop << "(" << box_if_inspectable(ctrl, pk, resolved) << ");\n";
                              }
                              else
                              {
                                  std::string pk_lo2 = pk;
                                  for (auto& c : pk_lo2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                  // The JSON key may be an xaml2winuic alias (foregroundColor)
                                  // that ctrl_prop mapped to the real setter (Foreground) —
                                  // family checks must match the mapped name too.
                                  std::string tpl_lo2 = tpl_prop;
                                  for (auto& c : tpl_lo2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                  bool skip_incompat = ((pk_lo2 == "data" || tpl_lo2 == "data")
                                      || (pk_lo2 == "foreground" || tpl_lo2 == "foreground")
                                      || (pk_lo2 == "fontfamily" || tpl_lo2 == "fontfamily")
                                      || (pk_lo2 == "iconsource" || tpl_lo2 == "iconsource")
                                      || (pk_lo2 == "icon" || tpl_lo2 == "icon"));
                                  bool empty_val = (collapsed.empty() || collapsed == "\"\"");
                                  bool has_xaml_res = (collapsed.find('{') != std::string::npos);
                                  if (skip_incompat && (empty_val || has_xaml_res))
                                      {} // skip incompatible props (empty or XAML resource markup)
                                  else
                                  {
                                       // Check if this is a Brush property receiving a color string
                                       bool is_brush_prop = ((pk_lo2 == "foreground" || tpl_lo2 == "foreground")
                                           || (pk_lo2 == "background" || tpl_lo2 == "background")
                                           || (pk_lo2 == "fill" || tpl_lo2 == "fill")
                                           || (pk_lo2 == "stroke" || tpl_lo2 == "stroke")
                                           || (pk_lo2 == "borderbrush" || tpl_lo2 == "borderbrush"));
                                        if (is_brush_prop && !collapsed.empty() && collapsed[0] != '{')
                                            out << indent << var_name << "." << tpl_prop
                                                << "(make_solid_brush(named_color(\"" << collapsed << "\")));\n";
                                        else
                                            out << indent << var_name << "." << tpl_prop
                                                << "(L\"" << collapsed << "\");\n";
                                   }
                              }
                          }
                      }
                      if (pp.pos < pp.s.size() && pp.s[pp.pos] == ',') pp.advance();
                  }
              } break;
          }
          pp.skip_ws(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == ':') pp.advance();
          pp.skip_value(); if (pp.pos < pp.s.size() && pp.s[pp.pos] == ',') pp.advance();
      }
    }

    // Parse the "raw" attribute arrays ([["Key","value"],...]) and emit the
    // statically-resolvable entries (Margin/Padding/Grid.Row/LineHeight/...).
    // Markup extensions ({ThemeResource}/{StaticResource}/{x:Bind}/...), event
    // handlers, styles and XAML fragments stay skipped.
    {
        auto lo8 = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        auto starts_with = [](std::string const& s, char const* p) { return s.rfind(p, 0) == 0; };
        auto ends_with = [](std::string const& s, char const* p) { std::string_view sv(s), pp(p); return sv.size() >= pp.size() && sv.substr(sv.size() - pp.size()) == pp; };
        auto is_all_num = [](std::string const& s) {
            if (s.empty()) return false;
            for (unsigned char c : s) if (!(std::isdigit(c) || c == ',' || c == '.' || c == '-' || c == ' ' || c == '\t')) return false;
            return true;
        };
        // Leaf controls that carry no layout/look properties in this projection.
        auto is_leaf = [](std::string const& ctrl_lo) {
            static char const* leafs[] = { "fonticon","symbolicon","pathicon","bitmapicon","imageicon",
                "fonticonsource","symboliconsource","pathiconsource","bitmapiconsource","imageiconsource",
                "geometrygroup","xamluicommand","implicitanimationset","swipeitem",nullptr };
            for (int i = 0; leafs[i]; ++i) if (ctrl_lo == leafs[i]) return true;
            return false;
        };
        char const* evt_suffixes[] = { "changed","click","invoked","loaded","tapped","dropped",
            "keydown","keyup","entered","exited","holding","pressed","selected","requested",nullptr };
        auto emit_raw = [&](std::string const& rk, std::string const& rv) {
            if (rv.empty() || rv[0] == '\"') return;
            std::string klo = lo8(rk);
            std::string v = rv;
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            std::string cl = lo8(ctrl);
            if (is_leaf(cl) && klo != "foreground" && klo != "background") return;
            // Template-carrying attrs (XML <DataTemplate...> payloads) are
            // resource definitions - not settable properties of the control.
            if (klo.find("template") != std::string::npos) return;
            if (starts_with(klo, "x:") || starts_with(klo, "automationproperties.")
                || starts_with(klo, "tooltipservice.") || klo == "mode" || klo == "command"
                || klo == "commandparameter" || klo == "contenttemplate" || klo == "itemtemplate"
                || klo == "navigateuri"
                || klo == "stretchdirection") return;
            for (int ei = 0; evt_suffixes[ei]; ++ei)
                if (ends_with(klo, evt_suffixes[ei])) return;
            std::string prop = rk; // PascalCase XAML attr name = setter name

            // Grid ColumnDefinitions / RowDefinitions: parse <ColumnDefinition Width="..."/> XML
            if ((klo == "columndefinitions" || klo == "rowdefinitions")
                && cl == "grid" && !v.empty() && v.front() == '<') {
                std::vector<std::string> dims;
                bool is_col = (klo == "columndefinitions");
                std::string dim_attr = is_col ? "Width=\"" : "Height=\"";
                std::string tmp = v;
                while (true) {
                    auto wp = tmp.find(dim_attr);
                    if (wp == std::string::npos) break;
                    wp += dim_attr.size();
                    auto we = tmp.find('"', wp);
                    if (we == std::string::npos) break;
                    dims.push_back(tmp.substr(wp, we - wp));
                    tmp = tmp.substr(we + 1);
                }
                if (!dims.empty()) {
                    std::string prop_name = is_col ? "ColumnDefinitions" : "RowDefinitions";
                    std::string def_type = is_col ? "ColumnDefinition" : "RowDefinition";
                    std::string put_fn = is_col ? "put_Width" : "put_Height";
                    out << indent << "{\n";
                    out << indent << "    auto __gc = " << var_name << ".get_" << prop_name << "();\n";
                    for (auto const& w : dims) {
                        if (w == "Auto") {
                            out << indent << "    { auto __d = winrt::Microsoft::UI::Xaml::Controls::" << def_type << "();\n";
                            out << indent << "      __d." << put_fn << "(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));\n";
                            out << indent << "      __gc.Append(__d); }\n";
                        } else if (!w.empty() && w.back() == '*') {
                            std::string num = w.substr(0, w.size() - 1);
                            if (num.empty()) num = "1";
                            out << indent << "    { auto __d = winrt::Microsoft::UI::Xaml::Controls::" << def_type << "();\n";
                            out << indent << "      __d." << put_fn << "(winrt::Microsoft::UI::Xaml::GridLength(" << num
                                << ", winrt::Microsoft::UI::Xaml::GridUnitType::Star));\n";
                            out << indent << "      __gc.Append(__d); }\n";
                        } else {
                            out << indent << "    { auto __d = winrt::Microsoft::UI::Xaml::Controls::" << def_type << "();\n";
                            out << indent << "      __d." << put_fn << "(winrt::Microsoft::UI::Xaml::GridLength(" << w
                                << ", winrt::Microsoft::UI::Xaml::GridUnitType::Pixel));\n";
                            out << indent << "      __gc.Append(__d); }\n";
                        }
                    }
                    out << indent << "}\n";
                }
                return;
            }

            // Handle {ThemeResource Key} and {StaticResource Key} values
            if (v.size() > 2 && v.front() == '{' && v.back() == '}') {
                std::string inner = v.substr(1, v.size() - 2);
                while (!inner.empty() && inner.front() == ' ') inner.erase(0, 1);
                while (!inner.empty() && inner.back() == ' ') inner.pop_back();
                // Extract resource key: "ThemeResource KeyName" or "StaticResource KeyName"
                std::string res_key;
                bool is_theme = false;
                bool is_static = false;
                {
                    std::string lo_inner = lo8(inner);
                    if (lo_inner.rfind("themeresource ", 0) == 0) {
                        is_theme = true;
                        res_key = inner.substr(14);
                    } else if (lo_inner.rfind("staticresource ", 0) == 0) {
                        is_static = true;
                        res_key = inner.substr(15);
                    }
                    while (!res_key.empty() && res_key.front() == ' ') res_key.erase(0, 1);
                    while (!res_key.empty() && res_key.back() == ' ') res_key.pop_back();
                }
                if (!res_key.empty() && (is_theme || is_static)) {
                    // Determine if this property expects a Brush or a Style/other
                    bool is_brush_prop = (klo == "background" || klo == "foreground"
                        || klo == "borderbrush" || klo == "fill" || klo == "stroke"
                        || klo == "overlaybrush" || klo == "selectedhighlightcolor"
                        || klo == "rowbackground" || klo == "alternatingrowbackground"
                        || klo == "axislinebrush" || klo == "tickbrush"
                        || klo == "gridlinemajorbrush" || klo == "gridlineminorbrush"
                        || klo == "datamarkerbrush" || klo == "databrush");
                    bool is_style_prop = (klo == "style");
                    bool is_template_prop = klo.ends_with("template");
                    if (is_brush_prop) {
                        out << indent << var_name << "." << prop
                            << "(theme_brush_th(L\"" << escape_wstr(res_key) << "\"));\n";
                    } else if (is_style_prop) {
                        out << indent << var_name << "." << prop
                            << "(style_lookup_th(L\"" << escape_wstr(res_key) << "\"));\n";
                    } else if (is_template_prop) {
                        // Template-typed setters need DataTemplate, not IInspectable.
                        out << indent << var_name << "." << prop
                            << "(data_template_th(L\"" << escape_wstr(res_key) << "\"));\n";
                    } else {
                        // Generic resource lookup — use as IInspectable where needed
                        // For content/header/tag/title/description properties, use the
                        // resolved value as a boxed string
                        bool isInspectable = (klo == "content" || klo == "header" || klo == "tag"
                            || klo == "title" || klo == "description" || klo == "watermark"
                            || klo == "itemssource" || klo == "child" || klo == "data");
                        if (isInspectable) {
                            out << indent << "{\n";
                            out << indent << "    auto __res = theme_lookup_th(L\"" << escape_wstr(res_key)
                                << "\", winrt::IInspectable{});\n";
                            out << indent << "    if (__res) " << var_name << "." << prop << "(__res);\n";
                            out << indent << "}\n";
                        }
                        // For non-inspectable generic resources, skip silently
                    }
                }
                return;
            }

            // Grid attached properties
            if (starts_with(klo, "grid.")) {
                std::string which = klo.substr(5);
                if ((which == "row" || which == "column" || which == "rowspan" || which == "columnspan")
                    && !v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-'))
                {
                    std::string m = (which == "row") ? "Row" : (which == "column") ? "Column"
                        : (which == "rowspan") ? "RowSpan" : "ColumnSpan";
                    out << indent << "Grid::Set" << m << "(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed("
                        << var_name << ".m_unknown.get()), " << v << ");\n";
                }
                return;
            }
            if (klo.find('.') != std::string::npos) return; // other attached props
            // Thickness/CornerRadius families (any non-leaf control)
            if (klo == "margin" || klo == "padding" || klo == "borderthickness" || klo == "cornerradius") {
                if (!is_all_num(v)) return;
                std::string resolved = resolve_prop_value(rk, v);
                if (!resolved.empty())
                    out << indent << var_name << "." << prop << "(" << resolved << ");\n";
                return;
            }
            // Plain numeric layout props
            if (klo == "lineheight" || klo == "fontsize" || klo == "minwidth" || klo == "minheight"
                || klo == "maxwidth" || klo == "maxheight" || klo == "width" || klo == "height"
                || klo == "maxlines") {
                bool text_only = (klo == "lineheight" || klo == "maxlines");
                if (text_only && cl != "textblock" && cl != "richtextblock") return;
                if (!is_all_num(v) || v.find(',') != std::string::npos) return;
                out << indent << var_name << "." << prop << "(" << v << ");\n";
                return;
            }
            // bool props (XAML uses capitalized True/False - normalize)
            std::string vlo = lo8(v);
            if (vlo == "true" || vlo == "false") {
                if (klo == "isexpanded" && cl == "treeviewitem")
                    out << indent << var_name << "." << prop << "(" << vlo << ");\n";
                return;
            }
            // TextBlock text enums
            if (cl == "textblock" || cl == "richtextblock") {
                if (klo == "texttrimming") {
                    if (v == "CharacterEllipsis") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);\n";
                    else if (v == "Clip") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::TextTrimming::Clip);\n";
                    else if (v == "None") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::TextTrimming::None);\n";
                    return;
                }
                if (klo == "textwrapping") {
                    if (v == "Wrap") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);\n";
                    else if (v == "NoWrap") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap);\n";
                    else if (v == "WrapWholeWords") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);\n";
                    return;
                }
                if (klo == "fontstyle") {
                    if (v == "Italic") out << indent << var_name << "." << prop << "(winrt::Windows::UI::Text::FontStyle::Italic);\n";
                    else if (v == "Oblique") out << indent << var_name << "." << prop << "(winrt::Windows::UI::Text::FontStyle::Oblique);\n";
                    else if (v == "Normal") out << indent << var_name << "." << prop << "(winrt::Windows::UI::Text::FontStyle::Normal);\n";
                    return;
                }
            }
            // Stretch (Image / shape controls)
            if (klo == "stretch" && (cl == "image" || cl == "rectangle" || cl == "ellipse" || cl == "line"
                || cl == "path" || cl == "polygon" || cl == "polyline")) {
                if (v == "Fill") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::Media::Stretch::Fill);\n";
                else if (v == "Uniform") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::Media::Stretch::Uniform);\n";
                else if (v == "UniformToFill") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::Media::Stretch::UniformToFill);\n";
                else if (v == "None") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::Media::Stretch::None);\n";
                return;
            }
            // Visibility
            if (klo == "visibility") {
                if (v == "Visible") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::Visibility::Visible);\n";
                else if (v == "Collapsed") out << indent << var_name << "." << prop << "(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);\n";
                return;
            }
        };
        template_json_parser pr; pr.s = json; pr.pos = 0;
        pr.skip_ws(); if (pr.pos < pr.s.size() && pr.s[pr.pos] == '{') pr.advance();
        int raw_guard = 0;
        while (pr.pos < pr.s.size() && pr.s[pr.pos] != '}') {
            if (++raw_guard > 64) break; // parse-guard: never spin on malformed JSON
            std::string key = pr.parse_string();
            if (key == "raw") {
                pr.skip_ws(); if (pr.pos < pr.s.size() && pr.s[pr.pos] == ':') pr.advance();
                pr.skip_ws();
                if (pr.pos < pr.s.size() && pr.s[pr.pos] == '[') {
                    pr.advance();
                    int raw_inner_guard = 0;
                    while (pr.pos < pr.s.size() && pr.s[pr.pos] != ']') {
                        if (++raw_inner_guard > 256) break; // parse-guard: never spin on malformed JSON
                        pr.skip_ws(); if (pr.s[pr.pos] == ']') break;
                        if (pr.s[pr.pos] != '[') { pr.skip_value(); continue; }
                        pr.advance(); // '['
                        pr.skip_ws();
                        std::string rk = pr.parse_string();
                        pr.skip_ws(); if (pr.pos < pr.s.size() && pr.s[pr.pos] == ',') pr.advance();
                        pr.skip_ws();
                        std::string rv = pr.parse_string();
                        pr.skip_ws(); if (pr.pos < pr.s.size() && pr.s[pr.pos] == ']') pr.advance();
                        emit_raw(rk, rv);
                        if (pr.pos < pr.s.size() && pr.s[pr.pos] == ',') pr.advance();
                    }
                }
                break;
            }
            pr.skip_ws(); if (pr.pos < pr.s.size() && pr.s[pr.pos] == ':') pr.advance();
            pr.skip_value(); if (pr.pos < pr.s.size() && pr.s[pr.pos] == ',') pr.advance();
        }
    }

    // Parse children
    { template_json_parser pc; pc.s = json; pc.pos = 0;
      pc.skip_ws(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == '{') pc.advance();
      while (pc.pos < pc.s.size() && pc.s[pc.pos] != '}') {
          std::string key = pc.parse_string();
          if (key == "children") {
              pc.skip_ws(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == ':') pc.advance();
              pc.skip_ws(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == '[') {
                  pc.advance();
                  while (pc.pos < pc.s.size() && pc.s[pc.pos] != ']') {
                      pc.skip_ws(); if (pc.s[pc.pos] == ']') break;
                      std::size_t start = pc.pos; pc.skip_value(); std::size_t end = pc.pos;
                      std::string child_var = json_node_to_cpp(
                          pc.s.substr(start, end - start), out, indent, var_counter, item_var, catalog, item_kind);
                      if (!child_var.empty()) {
                          bool is_panel = (ctrl == "Grid" || ctrl == "StackPanel" ||
                                          ctrl == "Canvas" || ctrl == "RelativePanel" ||
                                          ctrl == "WrapPanel" || ctrl == "UniformGrid");
                          if (is_panel)
                              out << indent << var_name << ".Children().Append(" << child_var << ");\n";
                          else {
                              // Border uses Child(); ContentControl-derived use Content();
                              std::string scp = catalog.single_child_prop(ctrl);
                              if (scp.empty()) scp = "Content";
                              // Child() property needs UIElement cast since projected types are standalone structs
                              if (scp == "Child")
                                  out << indent << var_name << "." << scp << "(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << child_var << ".m_unknown.get()));\n";
                              else
                                  out << indent << var_name << "." << scp << "(" << child_var << ");\n";
                          }
                      }
                      if (pc.pos < pc.s.size() && pc.s[pc.pos] == ',') pc.advance();
                  }
              } break;
          }
          pc.skip_ws(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == ':') pc.advance();
          pc.skip_value(); if (pc.pos < pc.s.size() && pc.s[pc.pos] == ',') pc.advance();
      }
    }

    return var_name;
}

// Globals for component-type detection and string escaping

inline auto template_payload_to_cpp_factory(
    std::string_view payload, int template_index, attribute_catalog const& catalog
) -> template_factory_result
{
    template_json_parser p; p.s = payload; p.pos = 0;
    std::string item_type = p.extract_string("itemType");
    std::string func_name = "Make_Tpl_" + std::to_string(template_index);
    if (std::getenv("CPPWINC_DBG_PAYLOAD"))
        std::fprintf(stderr, "PAYLOADDBG tpl=%s itemType=[%s]\n%s\n", func_name.c_str(), item_type.c_str(), std::string(payload).c_str());

    std::ostringstream body;
    body << "    winrt::IInspectable\n";
    body << "    " << func_name << "(winrt::IInspectable const& __data)\n";
    body << "    {\n";

    std::string item_kind;
    if (item_type == "String" || item_type == "x:String")
    {
        item_kind = "hstring";
        body << "        auto __item = winrt::unbox_value<winrt::hstring>(__data);\n";
    }
    else if (item_type == "Int32" || item_type == "x:Int32")
    {
        item_kind = "int32";
        body << "        auto __item = winrt::unbox_value<int32_t>(__data);\n";
    }
    else if (item_type == "Double" || item_type == "x:Double")
    {
        item_kind = "double";
        body << "        auto __item = winrt::unbox_value<double>(__data);\n";
    }
    else if (!item_type.empty())
    {
        // Model items: only types that exist in the loaded metadata as platform
        // (non-app) classes can be cast at runtime.  Unprojected C# model types
        // (absent from the winmd) stay untyped - nothing to cast to.
        auto it_ns = g_type_ns.find(item_type);
        if (it_ns != g_type_ns.end() && !g_project_nss.count(it_ns->second))
            item_kind = "obj";
        body << "        auto __item = __data;\n";
    }
    else
        body << "        auto __item = __data;\n";

    int var_counter = 0;
    { template_json_parser pb; pb.s = payload; pb.pos = 0;
      pb.skip_ws(); if (pb.pos < pb.s.size() && pb.s[pb.pos] == '{') pb.advance();
      while (pb.pos < pb.s.size() && pb.s[pb.pos] != '}') {
          std::string key = pb.parse_string();
          if (key == "body") {
              pb.skip_ws(); if (pb.pos < pb.s.size() && pb.s[pb.pos] == ':') pb.advance();
              pb.skip_ws(); if (pb.pos < pb.s.size() && pb.s[pb.pos] == '[') {
                  pb.advance();
                  while (pb.pos < pb.s.size() && pb.s[pb.pos] != ']') {
                      pb.skip_ws(); if (pb.s[pb.pos] == ']') break;                      std::size_t start = pb.pos; pb.skip_value(); std::size_t end = pb.pos;
                      std::string root_var = json_node_to_cpp(
                          pb.s.substr(start, end - start), body,
                          "            ", var_counter, "__item", catalog, item_kind);
                      if (!root_var.empty())
                          body << "        return " << root_var << ";\n";
                      if (pb.pos < pb.s.size() && pb.s[pb.pos] == ',') pb.advance();
                  }
              } break;
          }
          pb.skip_ws(); if (pb.pos < pb.s.size() && pb.s[pb.pos] == ':') pb.advance();
          pb.skip_value(); if (pb.pos < pb.s.size() && pb.s[pb.pos] == ',') pb.advance();
      }
    }

    body << "        return nullptr;\n";
    body << "    }\n";

    return { func_name, body.str(), item_type };
}


inline void emit_runtime_helpers(std::ostringstream& out, bool have_windows_sdk)
{
    // SolidColorBrush helpers: construct a REAL Microsoft.UI.Xaml.Media.SolidColorBrush
    // through its projected factory constructor (P1) - the WinUI 3 way. The converting
    // template ctor on projected classes lets it flow into Brush-typed setters.
    // ThemeResource value helper: guarded Application.Resources lookup with a
    // literal fallback (baked from the source XAML value at codegen time).
    // Strings arrive quoted; brushes arrive as pre-built SolidColorBrush.
    // The ResourceDictionary's IMap<Object,Object> is reached by raw QI: the
    // projected ResourceDictionary exposes no keyed accessors, and there is no
    // closed-generic iid_of<IMap<IInspectable,IInspectable>> to name via
    // iface<I>(). The closed GUID is computed from the pinterface pattern with
    // type_signature-completed args exactly as the OS does. The map handle is
    // released manually (raw QI result); the first lookup arg doubles as the
    // unbox scratch so no wrapped result ever over-releases.
    // theme_lookup_th: guarded Application.Resources lookup that mirrors the
    // XAML resource resolution order. Framework theme brushes (e.g.
    // ApplicationPageBackgroundThemeBrush) do NOT live in the dictionary's
    // flat IMap - they live in ThemeDictionaries ("Light"/"Default") inside
    // the merged XamlControlsResources dictionary. Resolution order: flat map
    // -> ThemeDictionaries (Light, then Default) -> merged dictionaries
    // (recursive). All map/vector access goes through the projected wrappers
    // (their ABI internals handle slots/refcounts); keyed access uses raw
    // HasKey/Lookup via IMap<Object,Object> since no closed-generic iid_of
    // exists. The first lookup arg doubles as the unbox scratch so no wrapped
    // result ever over-releases.
    // WinRT IMap<K, V> ABI vtable layout (inherits IInspectable):
    //   vtable[0..5] = IUnknown / IInspectable
    //   vtable[6]   = Lookup
    //   vtable[7]   = get_Size
    //   vtable[8]   = HasKey
    //   vtable[9]   = GetView
    //   vtable[10]  = Insert
    //   vtable[11]  = Remove
    //   vtable[12]  = Clear
    out << "extern \"C\" {\n";
    out << "    __declspec(dllimport) void* __stdcall LoadLibraryExW(const wchar_t*, void*, unsigned long);\n";
    out << "    __declspec(dllimport) void* __stdcall GetProcAddress(void*, const char*);\n";
    out << "    __declspec(dllimport) int __stdcall FreeLibrary(void*);\n";
    out << "    struct ThemeState {\n";
    out << "        int theme_idx = 0;\n";
    out << "        int compact = 0;\n";
    out << "        unsigned long long window_id = 0;\n";
    out << "        void* main_hwnd = nullptr;\n";
    out << "        bool is_extended_titlebar = false;\n";
    out << "        void* root_element = nullptr;\n";
    out << "        void* card_bg = nullptr;\n";
    out << "        void* card_stroke = nullptr;\n";
    out << "        void* divider_stroke = nullptr;\n";
    out << "        void* text_primary = nullptr;\n";
    out << "        void* text_secondary = nullptr;\n";
    out << "        void* text_tertiary = nullptr;\n";
    out << "        void* text_disabled = nullptr;\n";
    out << "        void* text_on_accent = nullptr;\n";
    out << "        void* accent = nullptr;\n";
    out << "        void* page_bg = nullptr;\n";
    out << "        void* solid_bg = nullptr;\n";
    out << "        void* local_dict = nullptr;\n";
    out << "        void (*notify_theme_change)() = nullptr;\n";
    out << "        void (*set_nav_mode)(int) = nullptr;\n";
    out << "        void (*navigate_to)(const wchar_t*) = nullptr;\n";
    out << "    };\n";
    out << "    __declspec(selectany) ThemeState g_theme_state{};\n";
    out << "}\n\n";
    out << "static bool is_system_light_mode() {\n";
    out << "    try {\n";
    out << "        void* adv = LoadLibraryExW(L\"advapi32.dll\", nullptr, 0x00000800);\n";
    out << "        if (adv) {\n";
    out << "            auto pRegOpen = reinterpret_cast<long(__stdcall*)(void*, const wchar_t*, unsigned long, unsigned long, void**)>(GetProcAddress(adv, \"RegOpenKeyExW\"));\n";
    out << "            auto pRegQuery = reinterpret_cast<long(__stdcall*)(void*, const wchar_t*, unsigned long*, unsigned long*, unsigned char*, unsigned long*)>(GetProcAddress(adv, \"RegQueryValueExW\"));\n";
    out << "            auto pRegClose = reinterpret_cast<long(__stdcall*)(void*)>(GetProcAddress(adv, \"RegCloseKey\"));\n";
    out << "            if (pRegOpen && pRegQuery && pRegClose) {\n";
    out << "                void* hKey = nullptr;\n";
    out << "                if (pRegOpen(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x80000001ULL)), L\"Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Themes\\\\Personalize\", 0, 0x20019, &hKey) == 0) {\n";
    out << "                    unsigned long val = 0;\n";
    out << "                    unsigned long size = sizeof(val);\n";
    out << "                    unsigned long type = 0;\n";
    out << "                    long res = pRegQuery(hKey, L\"AppsUseLightTheme\", nullptr, &type, reinterpret_cast<unsigned char*>(&val), &size);\n";
    out << "                    pRegClose(hKey);\n";
    out << "                    FreeLibrary(adv);\n";
    out << "                    if (res == 0) return val != 0;\n";
    out << "                }\n";
    out << "            }\n";
    out << "            FreeLibrary(adv);\n";
    out << "        }\n";
    out << "    } catch (...) {}\n";
    out << "    return false;\n";
    out << "}\n\n";
    out << "static bool is_current_light_theme() {\n";
    out << "    if (g_theme_state.theme_idx == 1) return true;\n";
    out << "    if (g_theme_state.theme_idx == 2) return false;\n";
    out << "    return is_system_light_mode();\n";
    out << "}\n\n";
    out << "inline bool is_current_dark_theme() {\n";
    out << "    return !is_current_light_theme();\n";
    out << "}\n\n";
    out << "inline winrt::Microsoft::UI::Xaml::ElementTheme get_global_theme() {\n";
    out << "    if (g_theme_state.theme_idx == 1) return winrt::Microsoft::UI::Xaml::ElementTheme::Light;\n";
    out << "    if (g_theme_state.theme_idx == 2) return winrt::Microsoft::UI::Xaml::ElementTheme::Dark;\n";
    out << "    return is_system_light_mode() ? winrt::Microsoft::UI::Xaml::ElementTheme::Light : winrt::Microsoft::UI::Xaml::ElementTheme::Dark;\n";
    out << "}\n\n";
    out << "static void sync_titlebar_theme(bool is_dark) {\n";
    out << "    try {\n";
    out << "        if (g_theme_state.window_id != 0 && winrt::Microsoft::UI::Windowing::AppWindowTitleBar::IsCustomizationSupported()) {\n";
    out << "            winrt::Microsoft::UI::WindowId wid{ g_theme_state.window_id };\n";
    out << "            auto app_win = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(wid);\n";
    out << "            if (app_win.get_abi()) {\n";
    out << "                auto tb = app_win.TitleBar();\n";
    out << "                if (tb.get_abi()) {\n";
    out << "                    if (!g_theme_state.is_extended_titlebar) {\n";
    out << "                        tb.ResetToDefault();\n";
    out << "                        return;\n";
    out << "                    }\n";
    out << "                    tb.ButtonBackgroundColor(winrt::Windows::UI::Color{0, 0, 0, 0});\n";
    out << "                    tb.ButtonInactiveBackgroundColor(winrt::Windows::UI::Color{0, 0, 0, 0});\n";
    out << "                    if (is_dark) {\n";
    out << "                        tb.ButtonForegroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});\n";
    out << "                        tb.ButtonHoverForegroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});\n";
    out << "                        tb.ButtonHoverBackgroundColor(winrt::Windows::UI::Color{26, 255, 255, 255});\n";
    out << "                        tb.ButtonPressedForegroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});\n";
    out << "                        tb.ButtonPressedBackgroundColor(winrt::Windows::UI::Color{51, 255, 255, 255});\n";
    out << "                        tb.ButtonInactiveForegroundColor(winrt::Windows::UI::Color{140, 255, 255, 255});\n";
    out << "                    } else {\n";
    out << "                        tb.ButtonForegroundColor(winrt::Windows::UI::Color{255, 0, 0, 0});\n";
    out << "                        tb.ButtonHoverForegroundColor(winrt::Windows::UI::Color{255, 0, 0, 0});\n";
    out << "                        tb.ButtonHoverBackgroundColor(winrt::Windows::UI::Color{26, 0, 0, 0});\n";
    out << "                        tb.ButtonPressedForegroundColor(winrt::Windows::UI::Color{255, 0, 0, 0});\n";
    out << "                        tb.ButtonPressedBackgroundColor(winrt::Windows::UI::Color{51, 0, 0, 0});\n";
    out << "                        tb.ButtonInactiveForegroundColor(winrt::Windows::UI::Color{140, 0, 0, 0});\n";
    out << "                    }\n";
    out << "                }\n";
    out << "            }\n";
    out << "        }\n";
    out << "    } catch (...) {}\n";
    out << "}\n\n";
    out << "static void sync_window_theme(int is_dark) {\n";
    out << "    try {\n";
    out << "        void* hwnd = nullptr;\n";
    out << "        if (g_theme_state.main_hwnd) hwnd = g_theme_state.main_hwnd;\n";
    out << "        if (!hwnd && g_theme_state.window_id != 0) {\n";
    out << "            hwnd = reinterpret_cast<void*>(static_cast<std::uintptr_t>(g_theme_state.window_id));\n";
    out << "        }\n";
    out << "        if (!hwnd) hwnd = ::Windows::Win32::UI::WindowsAndMessaging::GetActiveWindow();\n";
    out << "        if (hwnd) {\n";
    out << "            void* user32 = LoadLibraryExW(L\"user32.dll\", nullptr, 0x00000800);\n";
    out << "            if (user32) {\n";
    out << "                auto pGetAncestor = reinterpret_cast<void*(__stdcall*)(void*, unsigned int)>(GetProcAddress(user32, \"GetAncestor\"));\n";
    out << "                if (pGetAncestor) {\n";
    out << "                    void* root = pGetAncestor(hwnd, 2); // GA_ROOT\n";
    out << "                    if (root) hwnd = root;\n";
    out << "                }\n";
    out << "            }\n";
    out << "            g_theme_state.main_hwnd = hwnd;\n";
    out << "            void* dwm = LoadLibraryExW(L\"dwmapi.dll\", nullptr, 0x00000800);\n";
    out << "            if (dwm) {\n";
    out << "                auto pDwmSet = reinterpret_cast<long(__stdcall*)(void*, unsigned long, const void*, unsigned long)>(GetProcAddress(dwm, \"DwmSetWindowAttribute\"));\n";
    out << "                if (pDwmSet) {\n";
    out << "                    if (pDwmSet(hwnd, 20, &is_dark, sizeof(is_dark)) != 0) {\n";
    out << "                        pDwmSet(hwnd, 19, &is_dark, sizeof(is_dark)); // fallback for Win10 1809-1909\n";
    out << "                    }\n";
    out << "                }\n";
    out << "                FreeLibrary(dwm);\n";
    out << "            }\n";
    out << "            void* hUx = LoadLibraryExW(L\"uxtheme.dll\", nullptr, 0x00000800);\n";
    out << "            if (hUx) {\n";
    out << "                auto pSet = reinterpret_cast<int(__stdcall*)(int)>(GetProcAddress(hUx, reinterpret_cast<const char*>(135)));\n";
    out << "                auto pAllow = reinterpret_cast<int(__stdcall*)(void*, int)>(GetProcAddress(hUx, reinterpret_cast<const char*>(133)));\n";
    out << "                auto pFlush = reinterpret_cast<void(__stdcall*)()>(GetProcAddress(hUx, reinterpret_cast<const char*>(136)));\n";
    out << "                if (pSet) pSet(is_dark ? 2 : 3);\n";
    out << "                if (pAllow) pAllow(hwnd, is_dark);\n";
    out << "                if (pFlush) pFlush();\n";
    out << "                FreeLibrary(hUx);\n";
    out << "            }\n";
    out << "            // Trigger instant non-client repaint so DWM updates titlebar immediately without maximize\n";
    out << "            if (user32) {\n";
    out << "                auto pSetWindowPos = reinterpret_cast<int(__stdcall*)(void*, void*, int, int, int, int, unsigned int)>(GetProcAddress(user32, \"SetWindowPos\"));\n";
    out << "                if (pSetWindowPos) {\n";
    out << "                    // SWP_NOSIZE (1) | SWP_NOMOVE (2) | SWP_NOZORDER (4) | SWP_NOACTIVATE (0x10) | SWP_FRAMECHANGED (0x20) = 0x0037\n";
    out << "                    pSetWindowPos(hwnd, nullptr, 0, 0, 0, 0, 0x0037);\n";
    out << "                }\n";
    out << "                auto pRedrawWindow = reinterpret_cast<int(__stdcall*)(void*, const void*, void*, unsigned int)>(GetProcAddress(user32, \"RedrawWindow\"));\n";
    out << "                if (pRedrawWindow) {\n";
    out << "                    // RDW_INVALIDATE (1) | RDW_FRAME (0x400) | RDW_UPDATENOW (0x100) | RDW_ALLCHILDREN (0x80) = 0x0581\n";
    out << "                    pRedrawWindow(hwnd, nullptr, nullptr, 0x0581);\n";
    out << "                }\n";
    out << "                FreeLibrary(user32);\n";
    out << "            }\n";
    out << "        }\n";
    out << "    } catch (...) {}\n";
    out << "}\n\n";
    out << "static void update_dynamic_brushes() {\n";
    out << "    bool is_light = is_current_light_theme();\n";
    out << "    auto update_brush = [](void* raw_abi, winrt::Windows::UI::Color c) {\n";
    out << "        if (!raw_abi) return;\n";
    out << "        winrt::IUnknown::addref_raw(raw_abi);\n";
    out << "        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush b = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush::from_abi(raw_abi);\n";
    out << "        b.Color(c);\n";
    out << "    };\n";
    out << "    update_brush(g_theme_state.card_bg, is_light ? winrt::Windows::UI::Color{255, 255, 255, 255} : winrt::Windows::UI::Color{255, 43, 43, 43});\n";
    out << "    update_brush(g_theme_state.card_stroke, is_light ? winrt::Windows::UI::Color{255, 229, 229, 229} : winrt::Windows::UI::Color{255, 58, 58, 58});\n";
    out << "    update_brush(g_theme_state.divider_stroke, is_light ? winrt::Windows::UI::Color{255, 229, 229, 229} : winrt::Windows::UI::Color{255, 54, 54, 54});\n";
    out << "    update_brush(g_theme_state.text_primary, is_light ? winrt::Windows::UI::Color{255, 26, 26, 26} : winrt::Windows::UI::Color{255, 255, 255, 255});\n";
    out << "    update_brush(g_theme_state.text_secondary, is_light ? winrt::Windows::UI::Color{255, 92, 92, 92} : winrt::Windows::UI::Color{255, 208, 208, 208});\n";
    out << "    update_brush(g_theme_state.text_tertiary, is_light ? winrt::Windows::UI::Color{255, 118, 118, 118} : winrt::Windows::UI::Color{255, 160, 160, 160});\n";
    out << "    update_brush(g_theme_state.text_disabled, is_light ? winrt::Windows::UI::Color{255, 160, 160, 160} : winrt::Windows::UI::Color{255, 110, 110, 110});\n";
    out << "    update_brush(g_theme_state.text_on_accent, is_light ? winrt::Windows::UI::Color{255, 255, 255, 255} : winrt::Windows::UI::Color{255, 0, 0, 0});\n";
    out << "    update_brush(g_theme_state.accent, is_light ? winrt::Windows::UI::Color{255, 0, 95, 184} : winrt::Windows::UI::Color{255, 96, 205, 255});\n";
    out << "    update_brush(g_theme_state.page_bg, winrt::Windows::UI::Color{0, 0, 0, 0});\n";
    out << "    update_brush(g_theme_state.solid_bg, is_light ? winrt::Windows::UI::Color{255, 243, 243, 243} : winrt::Windows::UI::Color{255, 32, 32, 32});\n";
    out << "}\n\n";
    out << "static void apply_app_theme(bool is_dark) {\n";
    out << "    g_theme_state.theme_idx = is_dark ? 2 : 1;\n";
    out << "    sync_titlebar_theme(is_dark);\n";
    out << "    sync_window_theme(is_dark ? 1 : 0);\n";
    out << "    update_dynamic_brushes();\n";
    out << "    try {\n";
    out << "        if (g_theme_state.root_element) {\n";
    out << "            winrt::IUnknown::addref_raw(g_theme_state.root_element);\n";
    out << "            winrt::Microsoft::UI::Xaml::FrameworkElement fe = winrt::Microsoft::UI::Xaml::FrameworkElement::from_abi(g_theme_state.root_element);\n";
    out << "            fe.RequestedTheme(is_dark ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark : winrt::Microsoft::UI::Xaml::ElementTheme::Light);\n";
    out << "        }\n";
    out << "    } catch (...) {}\n";
    out << "    if (g_theme_state.notify_theme_change) g_theme_state.notify_theme_change();\n";
    out << "}\n\n";
    out << "static winrt::IInspectable theme_find_in_raw_map(void* map_raw, wchar_t const* key) {\n";
    out << "    if (!map_raw) return winrt::IInspectable{};\n";
    out << "    auto* map_vt = *reinterpret_cast<void* const**>(map_raw);\n";
    out << "    winrt::IInspectable key_box = winrt::box_value(winrt::hstring(key));\n";
    out << "    using has_key_t = std::int32_t(__stdcall*)(void*, void*, std::uint8_t*);\n";
    out << "    std::uint8_t has = 0;\n";
    out << "    if (reinterpret_cast<has_key_t>(map_vt[8])(map_raw, key_box.get_abi(), &has) < 0 || !has) return winrt::IInspectable{};\n";
    out << "    using lookup_t = std::int32_t(__stdcall*)(void*, void*, void**);\n";
    out << "    winrt::IInspectable scratch{};\n";
    out << "    if (reinterpret_cast<lookup_t>(map_vt[6])(map_raw, key_box.get_abi(), scratch.put()) < 0) return winrt::IInspectable{};\n";
    out << "    return scratch;\n";
    out << "}\n\n";
    out << "static winrt::IInspectable theme_find_in_obj(winrt::IInspectable const& obj, wchar_t const* key);\n\n";
    out << "static winrt::IInspectable theme_find_in_dict(winrt::Microsoft::UI::Xaml::ResourceDictionary const& rd, wchar_t const* key) {\n";
    out << "    if (!rd.m_unknown.get()) return winrt::IInspectable{};\n";
    out << "    return theme_find_in_obj(winrt::IInspectable(rd.addref_borrowed()), key);\n";
    out << "}\n\n";
    out << "static winrt::IInspectable theme_find_in_obj(winrt::IInspectable const& obj, wchar_t const* key) {\n";
    out << "    if (!obj) return winrt::IInspectable{};\n";
    out << "    void* inspectable_raw = obj.get_abi();\n";
    out << "    if (!inspectable_raw) return winrt::IInspectable{};\n";
    out << "    static const winrt::winrt_guid iid_map1{0x1b0d3570,0x0877,0x5ec2,{0x8a,0x2c,0x1b,0x95,0x37,0x37,0x0a,0x4a}};\n";
    out << "    static const winrt::winrt_guid iid_map2{0x3C26D538,0x02B8,0x5C3B,{0x9A,0x25,0x8E,0x49,0x08,0x15,0xCA,0x3D}};\n";
    out << "    void* map_raw = nullptr;\n";
    out << "    if (winrt::impl::query_interface(inspectable_raw, iid_map1, &map_raw) < 0 || !map_raw) {\n";
    out << "        if (winrt::impl::query_interface(inspectable_raw, iid_map2, &map_raw) < 0 || !map_raw) return winrt::IInspectable{};\n";
    out << "    }\n";
    out << "    winrt::IInspectable result = theme_find_in_raw_map(map_raw, key);\n";
    out << "    auto* map_vt = *reinterpret_cast<void* const**>(map_raw);\n";
    out << "    using release_t = std::uint32_t(__stdcall*)(void*);\n";
    out << "    reinterpret_cast<release_t>(map_vt[2])(map_raw);\n";
    out << "    return result;\n";
    out << "}\n\n";
    out << "static winrt::IInspectable theme_search_rd_recursive(winrt::Microsoft::UI::Xaml::ResourceDictionary const& rd, wchar_t const* key, wchar_t const* th1, wchar_t const* th2, wchar_t const* th3, int depth = 0) {\n";
    out << "    if (depth > 5 || !rd.m_unknown.get()) return winrt::IInspectable{};\n";
    out << "    winrt::IInspectable v = theme_find_in_dict(rd, key);\n";
    out << "    if (v) return v;\n";
    out << "    auto themes = rd.get_ThemeDictionaries();\n";
    out << "    if (themes.get_abi() && themes.m_ptr) {\n";
    out << "        winrt::IInspectable tdv = theme_find_in_raw_map(themes.m_ptr, th1);\n";
    out << "        if (!tdv) tdv = theme_find_in_raw_map(themes.m_ptr, th2);\n";
    out << "        if (!tdv) tdv = theme_find_in_raw_map(themes.m_ptr, th3);\n";
    out << "        if (tdv) {\n";
    out << "            v = theme_find_in_obj(tdv, key);\n";
    out << "            if (v) return v;\n";
    out << "        }\n";
    out << "    }\n";
    out << "    auto merged = rd.get_MergedDictionaries();\n";
    out << "    if (merged.get_abi()) {\n";
    out << "        std::uint32_t count = merged.get_Size();\n";
    out << "        for (std::uint32_t i = 0; i < count; ++i) {\n";
    out << "            auto md_obj = merged.GetAt(i);\n";
    out << "            v = theme_search_rd_recursive(md_obj, key, th1, th2, th3, depth + 1);\n";
    out << "            if (v) return v;\n";
    out << "        }\n";
    out << "    }\n";
    out << "    return winrt::IInspectable{};\n";
    out << "}\n\n";
    out << "static winrt::IInspectable theme_lookup_th(wchar_t const* key, winrt::IInspectable fallback) {\n";
    out << "    if (!key) return fallback;\n";
    out << "    try {\n";
    out << "        bool is_light = is_current_light_theme();\n";
    out << "        wchar_t const* th1 = is_light ? L\"Light\" : L\"Dark\";\n";
    out << "        wchar_t const* th2 = L\"Default\";\n";
    out << "        wchar_t const* th3 = is_light ? L\"Dark\" : L\"Light\";\n";
    out << "        if (g_theme_state.local_dict) {\n";
    out << "            auto lrd = winrt::Microsoft::UI::Xaml::ResourceDictionary::wrap_borrowed(g_theme_state.local_dict);\n";
    out << "            winrt::IInspectable lv = theme_search_rd_recursive(lrd, key, th1, th2, th3, 0);\n";
    out << "            if (lv) return lv;\n";
    out << "        }\n";
    out << "        auto app = winrt::Microsoft::UI::Xaml::Application::get_Current();\n";
    out << "        if (app.get_abi() == nullptr) return fallback;\n";
    out << "        auto res = app.get_Resources();\n";
    out << "        if (res.get_abi() == nullptr) return fallback;\n";
    out << "        winrt::IInspectable v = theme_search_rd_recursive(res, key, th1, th2, th3, 0);\n";
    out << "        if (v) return v;\n";
    out << "    } catch (...) {}\n";
    out << "    if (key && (std::wstring_view(key) == L\"AppTitleName\" || std::wstring_view(key) == L\"AppTitle\")) {\n";
    out << "        return winrt::box_value(winrt::hstring(L\"WinUI Gallery\"));\n";
    out << "    }\n";
    out << "    return fallback;\n";
    out << "}\n\n";
    out << "static winrt::Microsoft::UI::Xaml::Media::Brush theme_brush_th(wchar_t const* key) {\n";
    out << "    bool is_light = is_current_light_theme();\n";
    out << "    std::wstring_view k{ key ? key : L\"\" };\n";
    out << "    auto get_or_create = [&](void*& slot, winrt::Windows::UI::Color light_c, winrt::Windows::UI::Color dark_c) -> winrt::Microsoft::UI::Xaml::Media::Brush {\n";
    out << "        if (!slot) {\n";
    out << "            winrt::Microsoft::UI::Xaml::Media::SolidColorBrush b{ is_light ? light_c : dark_c };\n";
    out << "            winrt::IUnknown::addref_raw(b.get_abi());\n";
    out << "            slot = b.get_abi();\n";
    out << "            return b;\n";
    out << "        }\n";
    out << "        winrt::IUnknown::addref_raw(slot);\n";
    out << "        return winrt::Microsoft::UI::Xaml::Media::Brush::from_abi(slot);\n";
    out << "    };\n";
    out << "    if (k.find(L\"Acrylic\") != std::wstring_view::npos) {\n";
    out << "        winrt::Microsoft::UI::Xaml::Media::AcrylicBrush ab{};\n";
    out << "        ab.TintColor(is_light ? winrt::Windows::UI::Color{255, 252, 252, 252} : winrt::Windows::UI::Color{255, 44, 44, 44});\n";
    out << "        ab.TintOpacity(is_light ? 0.0 : 0.15);\n";
    out << "        ab.FallbackColor(is_light ? winrt::Windows::UI::Color{255, 249, 249, 249} : winrt::Windows::UI::Color{255, 44, 44, 44});\n";
    out << "        return ab;\n";
    out << "    }\n";
    out << "    if (k.find(L\"TextOnAccent\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.text_on_accent, winrt::Windows::UI::Color{255, 255, 255, 255}, winrt::Windows::UI::Color{255, 0, 0, 0});\n";
    out << "    if (k.find(L\"Secondary\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.text_secondary, winrt::Windows::UI::Color{255, 92, 92, 92}, winrt::Windows::UI::Color{255, 208, 208, 208});\n";
    out << "    if (k.find(L\"Tertiary\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.text_tertiary, winrt::Windows::UI::Color{255, 118, 118, 118}, winrt::Windows::UI::Color{255, 160, 160, 160});\n";
    out << "    if (k.find(L\"Disabled\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.text_disabled, winrt::Windows::UI::Color{255, 160, 160, 160}, winrt::Windows::UI::Color{255, 110, 110, 110});\n";
    out << "    if (k.find(L\"TextFillColorPrimary\") != std::wstring_view::npos || k.find(L\"PrimaryText\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.text_primary, winrt::Windows::UI::Color{255, 26, 26, 26}, winrt::Windows::UI::Color{255, 255, 255, 255});\n";
    out << "    if (k.find(L\"Accent\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.accent, winrt::Windows::UI::Color{255, 0, 95, 184}, winrt::Windows::UI::Color{255, 96, 205, 255});\n";
    out << "    if (k.find(L\"DisplayBrush\") != std::wstring_view::npos || k.find(L\"SolidBackground\") != std::wstring_view::npos || k.find(L\"SmokeFill\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.solid_bg, winrt::Windows::UI::Color{255, 243, 243, 243}, winrt::Windows::UI::Color{255, 32, 32, 32});\n";
    out << "    if (k.find(L\"CardBackground\") != std::wstring_view::npos || k.find(L\"LayerFillColor\") != std::wstring_view::npos || k.find(L\"SubtleFill\") != std::wstring_view::npos || k.find(L\"ControlFill\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.card_bg, winrt::Windows::UI::Color{255, 255, 255, 255}, winrt::Windows::UI::Color{255, 43, 43, 43});\n";
    out << "    if (k.find(L\"CardStroke\") != std::wstring_view::npos || k.find(L\"SurfaceStroke\") != std::wstring_view::npos || k.find(L\"ControlStroke\") != std::wstring_view::npos || k.find(L\"Border\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.card_stroke, winrt::Windows::UI::Color{255, 229, 229, 229}, winrt::Windows::UI::Color{255, 58, 58, 58});\n";
    out << "    if (k.find(L\"DividerStroke\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.divider_stroke, winrt::Windows::UI::Color{255, 229, 229, 229}, winrt::Windows::UI::Color{255, 54, 54, 54});\n";
    out << "    if (k.find(L\"ApplicationPageBackground\") != std::wstring_view::npos || k.find(L\"WindowBackground\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.page_bg, winrt::Windows::UI::Color{0, 0, 0, 0}, winrt::Windows::UI::Color{0, 0, 0, 0});\n";
    out << "    winrt::IInspectable v = theme_lookup_th(key, winrt::IInspectable{});\n";
    out << "    if (v) {\n";
    out << "        winrt::IUnknown::addref_raw(v.get_abi());\n";
    out << "        return winrt::Microsoft::UI::Xaml::Media::Brush::from_abi(v.get_abi());\n";
    out << "    }\n";
    out << "    if (k.find(L\"Background\") != std::wstring_view::npos || k.find(L\"Fill\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.card_bg, winrt::Windows::UI::Color{255, 255, 255, 255}, winrt::Windows::UI::Color{255, 43, 43, 43});\n";
    out << "    return get_or_create(g_theme_state.text_primary, winrt::Windows::UI::Color{255, 26, 26, 26}, winrt::Windows::UI::Color{255, 255, 255, 255});\n";
    out << "}\n\n";
    // Style lookup (for {StaticResource Key} style references): walks the same
    // flat/merged resolution as theme_lookup_th. Returns an empty wrapper when
    // missing (a null Style merely clears the property - no factory, no crash).
    out << "static winrt::Microsoft::UI::Xaml::Style style_lookup_th(wchar_t const* key) {\n";
    out << "    winrt::IInspectable v = theme_lookup_th(key, winrt::IInspectable{});\n";
    out << "    if (v) {\n";
    out << "        winrt::IUnknown::addref_raw(v.get_abi());\n";
    out << "        return winrt::Microsoft::UI::Xaml::Style::from_abi(v.get_abi());\n";
    out << "    }\n";
    out << "    if (key) {\n";
    out << "        std::wstring_view sv(key);\n";
    out << "        if (sv == L\"TokenViewSelectorBarStyle\") {\n";
    out << "            static void* cached_bar = nullptr;\n";
    out << "            if (cached_bar) { winrt::IUnknown::addref_raw(cached_bar); return winrt::Microsoft::UI::Xaml::Style::from_abi(cached_bar); }\n";
    out << "            try {\n";
    out << "                static const wchar_t* const s_xaml = L\"<Style xmlns=\\\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\\\" TargetType=\\\"SelectorBar\\\"><Setter Property=\\\"HorizontalAlignment\\\" Value=\\\"Left\\\"/><Setter Property=\\\"VerticalAlignment\\\" Value=\\\"Top\\\"/><Setter Property=\\\"IsTabStop\\\" Value=\\\"False\\\"/><Setter Property=\\\"TabNavigation\\\" Value=\\\"Once\\\"/><Setter Property=\\\"Template\\\"><Setter.Value><ControlTemplate TargetType=\\\"SelectorBar\\\"><Grid Background=\\\"{TemplateBinding Background}\\\" BorderBrush=\\\"{TemplateBinding BorderBrush}\\\" CornerRadius=\\\"{TemplateBinding CornerRadius}\\\"><ItemsView x:Name=\\\"PART_ItemsView\\\" MaxWidth=\\\"{TemplateBinding MaxWidth}\\\" MaxHeight=\\\"{TemplateBinding MaxHeight}\\\" Padding=\\\"{TemplateBinding Padding}\\\" ItemsSource=\\\"{TemplateBinding Items}\\\" TabNavigation=\\\"{TemplateBinding TabNavigation}\\\"><ItemsView.Layout><StackLayout Orientation=\\\"Horizontal\\\" Spacing=\\\"8\\\"/></ItemsView.Layout></ItemsView></Grid></ControlTemplate></Setter.Value></Setter></Style>\";\n";
    out << "                auto obj = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::hstring(s_xaml));\n";
    out << "                if (obj) { cached_bar = obj.get_abi(); winrt::IUnknown::addref_raw(cached_bar); return winrt::Microsoft::UI::Xaml::Style::wrap_borrowed(cached_bar); }\n";
    out << "            } catch (...) {}\n";
    out << "        } else if (sv == L\"TokenViewSelectorBarItemStyle\") {\n";
    out << "            static void* cached_item = nullptr;\n";
    out << "            if (cached_item) { winrt::IUnknown::addref_raw(cached_item); return winrt::Microsoft::UI::Xaml::Style::from_abi(cached_item); }\n";
    out << "            try {\n";
    out << "                static const wchar_t* const item_xaml = L\"<Style xmlns=\\\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\\\" TargetType=\\\"SelectorBarItem\\\"><Setter Property=\\\"BackgroundSizing\\\" Value=\\\"OuterBorderEdge\\\"/><Setter Property=\\\"Background\\\" Value=\\\"#353535\\\"/><Setter Property=\\\"Foreground\\\" Value=\\\"#FFFFFF\\\"/><Setter Property=\\\"BorderBrush\\\" Value=\\\"#404040\\\"/><Setter Property=\\\"BorderThickness\\\" Value=\\\"1\\\"/><Setter Property=\\\"Padding\\\" Value=\\\"23,5,23,6\\\"/><Setter Property=\\\"CornerRadius\\\" Value=\\\"16\\\"/><Setter Property=\\\"HorizontalAlignment\\\" Value=\\\"Left\\\"/><Setter Property=\\\"VerticalAlignment\\\" Value=\\\"Center\\\"/><Setter Property=\\\"Template\\\"><Setter.Value><ControlTemplate TargetType=\\\"SelectorBarItem\\\"><Grid x:Name=\\\"PART_ContainerRoot\\\" Background=\\\"{TemplateBinding Background}\\\" BackgroundSizing=\\\"{TemplateBinding BackgroundSizing}\\\" BorderBrush=\\\"{TemplateBinding BorderBrush}\\\" BorderThickness=\\\"{TemplateBinding BorderThickness}\\\" CornerRadius=\\\"{TemplateBinding CornerRadius}\\\"><Grid.Resources><SolidColorBrush x:Key=\\\"SelectedForegroundBrush\\\" Color=\\\"#000000\\\"/><SolidColorBrush x:Key=\\\"UnselectedForegroundBrush\\\" Color=\\\"#FFFFFF\\\"/><SolidColorBrush x:Key=\\\"SelectedBgBrush\\\" Color=\\\"#76B9ED\\\"/><SolidColorBrush x:Key=\\\"SelectedHoverBgBrush\\\" Color=\\\"#6BA8D6\\\"/><SolidColorBrush x:Key=\\\"UnselectedNormalBgBrush\\\" Color=\\\"#353535\\\"/><SolidColorBrush x:Key=\\\"UnselectedHoverBgBrush\\\" Color=\\\"#404040\\\"/><SolidColorBrush x:Key=\\\"DarkBorderBrush\\\" Color=\\\"#404040\\\"/></Grid.Resources><StackPanel Margin=\\\"{TemplateBinding Padding}\\\" HorizontalAlignment=\\\"{TemplateBinding HorizontalContentAlignment}\\\" VerticalAlignment=\\\"{TemplateBinding VerticalContentAlignment}\\\" Orientation=\\\"Horizontal\\\" Spacing=\\\"6\\\"><ContentPresenter x:Name=\\\"PART_IconVisual\\\" Content=\\\"{TemplateBinding Icon}\\\" Foreground=\\\"{TemplateBinding Foreground}\\\"/><TextBlock x:Name=\\\"PART_TextVisual\\\" Margin=\\\"4,0,0,0\\\" FontSize=\\\"14\\\" Foreground=\\\"{TemplateBinding Foreground}\\\" Text=\\\"{TemplateBinding Text}\\\"/></StackPanel><VisualStateManager.VisualStateGroups><VisualStateGroup x:Name=\\\"CombinedStates\\\"><VisualState x:Name=\\\"UnselectedNormal\\\"><Storyboard><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_ContainerRoot\\\" Storyboard.TargetProperty=\\\"Background\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource UnselectedNormalBgBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_ContainerRoot\\\" Storyboard.TargetProperty=\\\"BorderBrush\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource DarkBorderBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_TextVisual\\\" Storyboard.TargetProperty=\\\"Foreground\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource UnselectedForegroundBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_IconVisual\\\" Storyboard.TargetProperty=\\\"Foreground\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource UnselectedForegroundBrush}\\\"/></ObjectAnimationUsingKeyFrames></Storyboard></VisualState><VisualState x:Name=\\\"UnselectedPointerOver\\\"><Storyboard><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_ContainerRoot\\\" Storyboard.TargetProperty=\\\"Background\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource UnselectedHoverBgBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_ContainerRoot\\\" Storyboard.TargetProperty=\\\"BorderBrush\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource DarkBorderBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_TextVisual\\\" Storyboard.TargetProperty=\\\"Foreground\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource UnselectedForegroundBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_IconVisual\\\" Storyboard.TargetProperty=\\\"Foreground\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource UnselectedForegroundBrush}\\\"/></ObjectAnimationUsingKeyFrames></Storyboard></VisualState><VisualState x:Name=\\\"SelectedNormal\\\"><Storyboard><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_ContainerRoot\\\" Storyboard.TargetProperty=\\\"Background\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource SelectedBgBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_ContainerRoot\\\" Storyboard.TargetProperty=\\\"BorderBrush\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource SelectedBgBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_TextVisual\\\" Storyboard.TargetProperty=\\\"Foreground\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource SelectedForegroundBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_IconVisual\\\" Storyboard.TargetProperty=\\\"Foreground\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource SelectedForegroundBrush}\\\"/></ObjectAnimationUsingKeyFrames></Storyboard></VisualState><VisualState x:Name=\\\"SelectedPointerOver\\\"><Storyboard><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_ContainerRoot\\\" Storyboard.TargetProperty=\\\"Background\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource SelectedHoverBgBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_ContainerRoot\\\" Storyboard.TargetProperty=\\\"BorderBrush\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource SelectedHoverBgBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_TextVisual\\\" Storyboard.TargetProperty=\\\"Foreground\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource SelectedForegroundBrush}\\\"/></ObjectAnimationUsingKeyFrames><ObjectAnimationUsingKeyFrames Storyboard.TargetName=\\\"PART_IconVisual\\\" Storyboard.TargetProperty=\\\"Foreground\\\"><DiscreteObjectKeyFrame KeyTime=\\\"0\\\" Value=\\\"{StaticResource SelectedForegroundBrush}\\\"/></ObjectAnimationUsingKeyFrames></Storyboard></VisualState></VisualStateGroup></VisualStateManager.VisualStateGroups></Grid></ControlTemplate></Setter.Value></Setter></Style>\";\n";
    out << "                auto obj = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::hstring(item_xaml));\n";
    out << "                if (obj) { cached_item = obj.get_abi(); winrt::IUnknown::addref_raw(cached_item); return winrt::Microsoft::UI::Xaml::Style::wrap_borrowed(cached_item); }\n";
    out << "            } catch (...) {}\n";
    out << "        }\n";
    out << "    }\n";
    out << "    return winrt::Microsoft::UI::Xaml::Style::from_abi(nullptr);\n";
    out << "}\n\n";
    // Enum-by-name helpers that take a state var directly (x:Bind style):
    // style_from_name(Var) -> Style (Style-typed setters), theme_string_th(Var)
    // -> hstring (Text-typed setters). Applied by the code-fix pass when a
    // state-var argument meets a Style/Text setter.
    out << "static winrt::Microsoft::UI::Xaml::Style style_from_name(winrt::hstring const& name)\n";
    out << "{\n";
    out << "    return style_lookup_th(name.c_str());\n";
    out << "}\n";
    out << "static winrt::hstring theme_string_th(winrt::hstring const& name)\n";
    out << "{\n";
    out << "    return name;\n";
    out << "}\n";
    // Overloads taking a raw resource lookup result (IInspectable): nesting
    // theme_lookup_th(...) inside the wchar_t*/hstring overloads is a compile
    // error — the resource object is used directly instead.
    out << "static winrt::Microsoft::UI::Xaml::Media::Brush theme_brush_th(winrt::IInspectable const& v)\n";
    out << "{\n";
    out << "    if (!v) return winrt::Microsoft::UI::Xaml::Media::Brush::from_abi(nullptr);\n";
    out << "    winrt::IUnknown::addref_raw(v.get_abi());\n";
    out << "    return winrt::Microsoft::UI::Xaml::Media::Brush::from_abi(v.get_abi());\n";
    out << "}\n";
    out << "static winrt::hstring theme_string_th(winrt::IInspectable const& v)\n";
    out << "{\n";
    out << "    return winrt::unbox_value_or<winrt::hstring>(v, winrt::hstring{});\n";
    out << "}\n\n";
    // Enum-by-name: a state var holds an alignment name ("Center", ...);
    // enum-typed setters need the projected enum value. Two concrete helpers
    // (the enums share no full member set, so a template cannot work).
    out << "static winrt::Microsoft::UI::Xaml::HorizontalAlignment halignment_from_name(winrt::hstring const& name)\n";
    out << "{\n";
    out << "    std::wstring_view sv = name.to_view();\n";
    out << "    auto cmp = [&](wchar_t const* lit) { return sv == std::wstring_view(lit); };\n";
    out << "    if (cmp(L\"Left\")) return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Left;\n";
    out << "    if (cmp(L\"Center\")) return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Center;\n";
    out << "    if (cmp(L\"Right\")) return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right;\n";
    out << "    return winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch;\n";
    out << "}\n";
    out << "static winrt::Microsoft::UI::Xaml::VerticalAlignment valignment_from_name(winrt::hstring const& name)\n";
    out << "{\n";
    out << "    std::wstring_view sv = name.to_view();\n";
    out << "    auto cmp = [&](wchar_t const* lit) { return sv == std::wstring_view(lit); };\n";
    out << "    if (cmp(L\"Top\")) return winrt::Microsoft::UI::Xaml::VerticalAlignment::Top;\n";
    out << "    if (cmp(L\"Center\")) return winrt::Microsoft::UI::Xaml::VerticalAlignment::Center;\n";
    out << "    if (cmp(L\"Bottom\")) return winrt::Microsoft::UI::Xaml::VerticalAlignment::Bottom;\n";
    out << "    return winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch;\n";
    out << "}\n\n";
    // String-state-var conversions for struct-typed setters: a @state string
    // holding "1,1,1,1" (or a single number) feeds Thickness/CornerRadius
    // setters through these parsers; bare strings would not compile.
    out << "static winrt::Microsoft::UI::Xaml::Thickness thickness_from_name(winrt::hstring const& name)\n";
    out << "{\n";
    out << "    double v[4] = { 0, 0, 0, 0 };\n";
    out << "    std::wstring_view sv = name.to_view();\n";
    out << "    int n = 0; size_t pos = 0;\n";
    out << "    while (n < 4 && pos <= sv.size()) {\n";
    out << "        size_t end = pos; double val = 0;\n";
    out << "        try { val = std::stod(std::wstring(sv.substr(pos)), &end); } catch (...) { break; }\n";
    out << "        v[n++] = val; pos += end;\n";
    out << "        while (pos < sv.size() && (sv[pos] == L',' || sv[pos] == L' ')) ++pos;\n";
    out << "        if (pos == end) break;\n    }\n";
    out << "    if (n == 1) return winrt::Microsoft::UI::Xaml::Thickness{ v[0], v[0], v[0], v[0] };\n";
    out << "    if (n == 2) return winrt::Microsoft::UI::Xaml::Thickness{ v[0], v[1], v[0], v[1] };\n";
    out << "    return winrt::Microsoft::UI::Xaml::Thickness{ v[0], v[1], v[2], v[3] };\n";
    out << "}\n";
    out << "static winrt::Microsoft::UI::Xaml::CornerRadius corner_from_name(winrt::hstring const& name)\n";
    out << "{\n";
    out << "    auto th = thickness_from_name(name);\n";
    out << "    return winrt::Microsoft::UI::Xaml::CornerRadius{ th.Left, th.Top, th.Right, th.Bottom };\n";
    out << "}\n\n";
    // DataTemplate-typed setters (ItemTemplate, ContentTemplate) take a
    // DataTemplate wrapper, not the raw IInspectable resource lookup result.
    out << "static winrt::Microsoft::UI::Xaml::DataTemplate data_template_th(wchar_t const* key) {\n";
    out << "    winrt::IInspectable v = theme_lookup_th(key, winrt::IInspectable{});\n";
    out << "    if (v) {\n";
    out << "        winrt::IUnknown::addref_raw(v.get_abi());\n";
    out << "        return winrt::Microsoft::UI::Xaml::DataTemplate::from_abi(v.get_abi());\n";
    out << "    }\n";
    out << "    if (key && std::wstring_view(key) == L\"ControlItemTemplate\") {\n";
    out << "        static void* cached_dt = nullptr;\n";
    out << "        if (cached_dt) { winrt::IUnknown::addref_raw(cached_dt); return winrt::Microsoft::UI::Xaml::DataTemplate::from_abi(cached_dt); }\n";
    out << "        try {\n";
    out << "            static const wchar_t* const dt_xaml = L\"<DataTemplate xmlns=\\\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\\\" xmlns:x=\\\"http://schemas.microsoft.com/winfx/2006/xaml\\\"><Grid x:Name=\\\"controlRoot\\\" Width=\\\"300\\\" Height=\\\"96\\\" Padding=\\\"8\\\" Background=\\\"#2B2B2B\\\" BorderBrush=\\\"#3A3A3A\\\" BorderThickness=\\\"1\\\" CornerRadius=\\\"8\\\"><Grid.ColumnDefinitions><ColumnDefinition Width=\\\"Auto\\\"/><ColumnDefinition Width=\\\"*\\\"/></Grid.ColumnDefinitions><Grid.RowDefinitions><RowDefinition Height=\\\"Auto\\\"/><RowDefinition Height=\\\"*\\\"/></Grid.RowDefinitions><Image x:Name=\\\"gridImage\\\" Grid.RowSpan=\\\"2\\\" Width=\\\"32\\\" Height=\\\"32\\\" Margin=\\\"8,12,16,0\\\" VerticalAlignment=\\\"Top\\\" Stretch=\\\"Uniform\\\" Source=\\\"{Binding ImagePath}\\\"/><TextBlock x:Name=\\\"titleText\\\" Grid.Column=\\\"1\\\" Margin=\\\"0,12,0,0\\\" VerticalAlignment=\\\"Bottom\\\" FontWeight=\\\"SemiBold\\\" FontSize=\\\"14\\\" Foreground=\\\"#FFFFFF\\\" TextTrimming=\\\"CharacterEllipsis\\\" TextWrapping=\\\"NoWrap\\\" Text=\\\"{Binding Title}\\\"/><TextBlock x:Name=\\\"subtitleText\\\" Grid.Row=\\\"1\\\" Grid.Column=\\\"1\\\" Margin=\\\"0,2,0,0\\\" FontSize=\\\"12\\\" Foreground=\\\"#9E9E9E\\\" TextTrimming=\\\"WordEllipsis\\\" TextWrapping=\\\"Wrap\\\" Text=\\\"{Binding Subtitle}\\\"/></Grid></DataTemplate>\";\n";
    out << "            auto obj = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::hstring(dt_xaml));\n";
    out << "            if (obj) { cached_dt = obj.get_abi(); winrt::IUnknown::addref_raw(cached_dt); return winrt::Microsoft::UI::Xaml::DataTemplate::wrap_borrowed(cached_dt); }\n";
    out << "        } catch (...) {}\n";
    out << "    }\n";
    out << "    return winrt::Microsoft::UI::Xaml::DataTemplate::from_abi(nullptr);\n";
    out << "}\n\n";
    out << "static winrt::Microsoft::UI::Xaml::Controls::ControlTemplate control_template_th(wchar_t const* key) {\n";
    out << "    winrt::IInspectable v = theme_lookup_th(key, winrt::IInspectable{});\n";
    out << "    if (!v) return winrt::Microsoft::UI::Xaml::Controls::ControlTemplate::from_abi(nullptr);\n";
    out << "    winrt::IUnknown::addref_raw(v.get_abi());\n";
    out << "    return winrt::Microsoft::UI::Xaml::Controls::ControlTemplate::from_abi(v.get_abi());\n";
    out << "}\n\n";
    out << "static winrt::Microsoft::UI::Xaml::Media::ImageSource image_source_lookup_th(wchar_t const* key) {\n";
    out << "    if (key && std::wstring_view(key) == L\"StoreIcon\") {\n";
    out << "        bool is_light = is_current_light_theme();\n";
    out << "        return winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(is_light ? L\"ms-appx:///Assets/HomeHeaderTiles/Header-Store.light.png\" : L\"ms-appx:///Assets/HomeHeaderTiles/Header-Store.dark.png\"));\n";
    out << "    }\n";
    out << "    winrt::IInspectable v = theme_lookup_th(key, winrt::IInspectable{});\n";
    out << "    if (!v) return winrt::Microsoft::UI::Xaml::Media::ImageSource::from_abi(nullptr);\n";
    out << "    void* is_raw = nullptr;\n";
    out << "    if (winrt::IUnknown::qi_raw(v.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Media::ImageSource>, &is_raw) >= 0 && is_raw) {\n";
    out << "        return winrt::Microsoft::UI::Xaml::Media::ImageSource::from_abi(is_raw);\n";
    out << "    }\n";
    out << "    winrt::hstring hs = winrt::unbox_value_or<winrt::hstring>(v, L\"\");\n";
    out << "    if (!hs.empty()) {\n";
    out << "        try {\n";
    out << "            std::wstring s = hs.c_str();\n";
    out << "            for (auto& c : s) if (c == L'\\\\') c = L'/';\n";
    out << "            if (s.rfind(L\"ms-appx:/\", 0) == 0 && s.rfind(L\"ms-appx:///\", 0) != 0) s = L\"ms-appx:///\" + s.substr(9);\n";
    out << "            else if (s.rfind(L\"/Assets/\", 0) == 0) s = L\"ms-appx:///\" + s.substr(1);\n";
    out << "            else if (s.rfind(L\"Assets/\", 0) == 0) s = L\"ms-appx:///\" + s;\n";
    out << "            return winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(s.c_str()));\n";
    out << "        } catch (...) {}\n";
    out << "    }\n";
    out << "    return winrt::Microsoft::UI::Xaml::Media::ImageSource::from_abi(nullptr);\n";
    out << "}\n\n";
    out << "static void apply_xaml_resources(winrt::Microsoft::UI::Xaml::FrameworkElement const& el, wchar_t const* xaml) {\n";
    out << "    if (!el.get_abi() || !xaml || !*xaml) return;\n";
    out << "    try {\n";
    out << "        std::wstring s = xaml;\n";
    out << "        size_t start = s.find_first_not_of(L\" \\t\\r\\n\"); if (start != std::wstring::npos) s = s.substr(start);\n";
    out << "        std::wstring full;\n";
    out << "        static const wchar_t* const xml_ns = L\" xmlns=\\\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\\\" xmlns:x=\\\"http://schemas.microsoft.com/winfx/2006/xaml\\\" xmlns:d=\\\"http://schemas.microsoft.com/expression/blend/2008\\\" xmlns:mc=\\\"http://schemas.openxmlformats.org/markup-compatibility/2006\\\" xmlns:local=\\\"using:WinUIGallery\\\" xmlns:controls=\\\"using:WinUIGallery.Controls\\\" xmlns:animations=\\\"using:CommunityToolkit.WinUI.Animations\\\" xmlns:media=\\\"using:CommunityToolkit.WinUI.Media\\\" xmlns:wuxdata=\\\"using:Microsoft.UI.Xaml.Data\\\"\";\n";
    out << "        if (s.rfind(L\"<ResourceDictionary\", 0) == 0) {\n";
    out << "            auto pos = s.find(L\">\", 19);\n";
    out << "            if (pos != std::wstring::npos) {\n";
    out << "                full = s.substr(0, 19) + xml_ns + s.substr(19);\n";
    out << "            } else { full = s; }\n";
    out << "        } else {\n";
    out << "            full = std::wstring(L\"<ResourceDictionary\") + xml_ns + L\">\" + s + L\"</ResourceDictionary>\";\n";
    out << "        }\n";
    out << "        auto obj = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::hstring(full.c_str()));\n";
    out << "        if (obj) {\n";
    out << "            auto rd = winrt::Microsoft::UI::Xaml::ResourceDictionary::wrap_borrowed(obj.get_abi());\n";
    out << "            if (rd.get_abi()) {\n";
    out << "                el.Resources().MergedDictionaries().Append(rd);\n";
    out << "                winrt::IUnknown::addref_raw(rd.get_abi());\n";
    out << "                g_theme_state.local_dict = rd.get_abi();\n";
    out << "            }\n";
    out << "        }\n";
    out << "    } catch (...) {}\n";
    out << "}\n\n";
    out << "static void insert_resource(winrt::Microsoft::UI::Xaml::ResourceDictionary const& rd, wchar_t const* key, winrt::IInspectable const& val) {\n";
    out << "    if (!rd.get_abi() || !key || !val.get_abi()) return;\n";
    out << "    static winrt::winrt_guid const imap_iid{ 0x1b0d3570, 0x0877, 0x5ec2, { 0x8a, 0x2c, 0x1b, 0x95, 0x37, 0x37, 0x0a, 0x4a } };\n";
    out << "    void* map_ptr = nullptr;\n";
    out << "    if (winrt::impl::query_interface(rd.get_abi(), imap_iid, &map_ptr) == 0 && map_ptr) {\n";
    out << "        void* boxed_key = winrt::box_value(winrt::hstring(key));\n";
    out << "        auto** vtbl = *reinterpret_cast<void***>(map_ptr);\n";
    out << "        using insert_fn = std::int32_t(__stdcall*)(void*, void*, void*, unsigned char*);\n";
    out << "        unsigned char replaced = 0;\n";
    out << "        reinterpret_cast<insert_fn>(vtbl[10])(map_ptr, boxed_key, val.get_abi(), &replaced);\n";
    out << "        if (boxed_key) {\n";
    out << "            auto** b_vtbl = *reinterpret_cast<void***>(boxed_key);\n";
    out << "            reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(b_vtbl[2])(boxed_key);\n";
    out << "        }\n";
    out << "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vtbl[2])(map_ptr);\n";
    out << "    }\n";
    out << "}\n\n";
    out << "static void apply_app_resources(winrt::Microsoft::UI::Xaml::Application const& app, wchar_t const* xaml) {\n";
    out << "    if (!app.get_abi() || !xaml || !*xaml) return;\n";
    out << "    try {\n";
    out << "        std::wstring s = xaml;\n";
    out << "        auto erase_sub = [&](std::wstring const& start_tag, std::wstring const& end_tag) {\n";
    out << "            size_t p = 0;\n";
    out << "            while ((p = s.find(start_tag, p)) != std::wstring::npos) {\n";
    out << "                auto end = s.find(end_tag, p);\n";
    out << "                if (end != std::wstring::npos) s.erase(p, end + end_tag.size() - p);\n";
    out << "                else break;\n";
    out << "            }\n";
    out << "        };\n";
    out << "        erase_sub(L\"<ResourceDictionary Source=\", L\"/>\");\n";
    out << "        erase_sub(L\"<local:ItemTemplates\", L\"/>\");\n";
    out << "        erase_sub(L\"<charts:\", L\"/>\");\n";
    out << "        erase_sub(L\"<TabularControlsResources\", L\"/>\");\n";
    out << "        erase_sub(L\"<converters:\", L\"/>\");\n";
    out << "        erase_sub(L\"<toolkitConverters:\", L\"</toolkitConverters:EmptyObjectToObjectConverter>\");\n";
    out << "        erase_sub(L\"<toolkitConverters:\", L\"/>\");\n";
    out << "        size_t bpos = 0;\n";
    out << "        while ((bpos = s.find(L\"{x:Bind \", bpos)) != std::wstring::npos) {\n";
    out << "            s.replace(bpos, 8, L\"{Binding \");\n";
    out << "            bpos += 9;\n";
    out << "        }\n";
    out << "        std::wstring_view sv(xaml);\n";
    out << "        size_t kpos = 0;\n";
    out << "        while ((kpos = sv.find(L\"x:Key=\\\"\", kpos)) != std::wstring_view::npos) {\n";
    out << "            size_t kstart = kpos + 7;\n";
    out << "            size_t kend = sv.find(L'\\\"', kstart);\n";
    out << "            if (kend == std::wstring_view::npos) break;\n";
    out << "            std::wstring key(sv.substr(kstart, kend - kstart));\n";
    out << "            size_t tag_close = sv.find(L'>', kend);\n";
    out << "            if (tag_close != std::wstring_view::npos && tag_close + 1 < sv.size() && sv[tag_close - 1] != L'/') {\n";
    out << "                size_t tag_end = sv.find(L'<', tag_close + 1);\n";
    out << "                if (tag_end != std::wstring_view::npos) {\n";
    out << "                    std::wstring val(sv.substr(tag_close + 1, tag_end - (tag_close + 1)));\n";
    out << "                    size_t tag_open = sv.rfind(L'<', kpos);\n";
    out << "                    if (tag_open != std::wstring_view::npos) {\n";
    out << "                        std::wstring_view tag_name = sv.substr(tag_open + 1, kpos - (tag_open + 1));\n";
    out << "                        if (tag_name.find(L\"Double\") != std::wstring_view::npos) {\n";
    out << "                            try { double d = std::stod(val); insert_resource(app.Resources(), key.c_str(), winrt::box_value(d)); } catch (...) {}\n";
    out << "                        } else if (tag_name.find(L\"String\") != std::wstring_view::npos) {\n";
    out << "                            insert_resource(app.Resources(), key.c_str(), winrt::box_value(winrt::hstring(val.c_str())));\n";
    out << "                        }\n";
    out << "                    }\n";
    out << "                }\n";
    out << "            }\n";
    out << "            kpos = kend + 1;\n";
    out << "        }\n";
    out << "        static const wchar_t* const xml_ns = L\" xmlns=\\\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\\\" xmlns:x=\\\"http://schemas.microsoft.com/winfx/2006/xaml\\\" xmlns:d=\\\"http://schemas.microsoft.com/expression/blend/2008\\\" xmlns:mc=\\\"http://schemas.openxmlformats.org/markup-compatibility/2006\\\" xmlns:local=\\\"using:WinUIGallery\\\" xmlns:controls=\\\"using:WinUIGallery.Controls\\\" xmlns:animations=\\\"using:CommunityToolkit.WinUI.Animations\\\" xmlns:media=\\\"using:CommunityToolkit.WinUI.Media\\\" xmlns:wuxdata=\\\"using:Microsoft.UI.Xaml.Data\\\" xmlns:models=\\\"using:WinUIGallery.Models\\\"\";\n";
    out << "        std::wstring full;\n";
    out << "        if (s.rfind(L\"<ResourceDictionary\", 0) == 0) {\n";
    out << "            auto pos = s.find(L\">\", 19);\n";
    out << "            if (pos != std::wstring::npos) full = s.substr(0, 19) + xml_ns + s.substr(19);\n";
    out << "            else full = s;\n";
    out << "        } else { full = std::wstring(L\"<ResourceDictionary\") + xml_ns + L\">\" + s + L\"</ResourceDictionary>\"; }\n";
    out << "        auto obj = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::hstring(full.c_str()));\n";
    out << "        if (obj) {\n";
    out << "            auto rd = winrt::Microsoft::UI::Xaml::ResourceDictionary::wrap_borrowed(obj.get_abi());\n";
    out << "            if (rd.get_abi()) { app.Resources().MergedDictionaries().Append(rd); (void)rd.m_unknown.detach(); }\n";
    out << "        }\n";
    out << "    } catch (...) {}\n";
    out << "}\n\n";
    out << "static void apply_visual_state_groups(winrt::Microsoft::UI::Xaml::FrameworkElement const& el, wchar_t const* xaml) {\n";
    out << "    if (!el.get_abi() || !xaml || !*xaml) return;\n";
    out << "    try {\n";
    out << "        static const wchar_t* const xml_ns = L\" xmlns=\\\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\\\" xmlns:x=\\\"http://schemas.microsoft.com/winfx/2006/xaml\\\" xmlns:d=\\\"http://schemas.microsoft.com/expression/blend/2008\\\" xmlns:mc=\\\"http://schemas.openxmlformats.org/markup-compatibility/2006\\\" xmlns:local=\\\"using:WinUIGallery\\\" xmlns:controls=\\\"using:WinUIGallery.Controls\\\" xmlns:animations=\\\"using:CommunityToolkit.WinUI.Animations\\\" xmlns:media=\\\"using:CommunityToolkit.WinUI.Media\\\" xmlns:wuxdata=\\\"using:Microsoft.UI.Xaml.Data\\\"\";\n";
    out << "        std::wstring full = std::wstring(L\"<Grid\") + xml_ns + L\"><VisualStateManager.VisualStateGroups>\" + std::wstring(xaml) + L\"</VisualStateManager.VisualStateGroups></Grid>\";\n";
    out << "        auto obj = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::hstring(full.c_str()));\n";
    out << "        if (obj) {\n";
    out << "            auto host = winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(obj.get_abi());\n";
    out << "            auto src = winrt::Microsoft::UI::Xaml::VisualStateManager::GetVisualStateGroups(host);\n";
    out << "            auto dst = winrt::Microsoft::UI::Xaml::VisualStateManager::GetVisualStateGroups(el);\n";
    out << "            auto count = src.get_Size();\n";
    out << "            std::vector<winrt::Microsoft::UI::Xaml::VisualStateGroup> list;\n";
    out << "            list.reserve(count);\n";
    out << "            for (uint32_t i = 0; i < count; ++i) list.push_back(src.GetAt(i));\n";
    out << "            src.Clear();\n";
    out << "            for (auto const& g : list) dst.Append(g);\n";
    out << "        }\n";
    out << "    } catch (...) {}\n";
    out << "}\n\n";
    out << "static winrt::Microsoft::UI::Xaml::Controls::ControlTemplate load_control_template(wchar_t const* xaml) {\n";
    out << "    if (!xaml || !*xaml) return winrt::Microsoft::UI::Xaml::Controls::ControlTemplate::from_abi(nullptr);\n";
    out << "    try {\n";
    out << "        std::wstring s = xaml;\n";
    out << "        size_t start = s.find_first_not_of(L\" \\t\\r\\n\"); if (start != std::wstring::npos) s = s.substr(start);\n";
    out << "        std::wstring full;\n";
    out << "        if (s.rfind(L\"<ControlTemplate\", 0) == 0) {\n";
    out << "            auto pos = s.find(L\">\", 16);\n";
    out << "            if (pos != std::wstring::npos) {\n";
    out << "                full = s.substr(0, 16) + L\" xmlns=\\\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\\\" xmlns:x=\\\"http://schemas.microsoft.com/winfx/2006/xaml\\\"\" + s.substr(16);\n";
    out << "            } else { full = s; }\n";
    out << "        } else {\n";
    out << "            full = L\"<ControlTemplate xmlns=\\\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\\\" xmlns:x=\\\"http://schemas.microsoft.com/winfx/2006/xaml\\\">\" + s + L\"</ControlTemplate>\";\n";
    out << "        }\n";
    out << "        auto obj = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::hstring(full.c_str()));\n";
    out << "        if (obj) return winrt::Microsoft::UI::Xaml::Controls::ControlTemplate::wrap_borrowed(obj.get_abi());\n";
    out << "    } catch (...) {}\n";
    out << "    return winrt::Microsoft::UI::Xaml::Controls::ControlTemplate::from_abi(nullptr);\n";
    out << "}\n\n";
    out << "static winrt::Microsoft::UI::Xaml::Media::SolidColorBrush make_solid_brush(winrt::Windows::UI::Color const& c) {\n";
    out << "    return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(c);\n";
    out << "}\n\n";
    // SolidColorBrush-typed setters (SelectionHighlightColor) take SolidColorBrush,
    // not the Brush base (projected structs are standalone).
    out << "static winrt::Microsoft::UI::Xaml::Media::SolidColorBrush solid_color_brush(winrt::Windows::UI::Color const& c) {\n";
    out << "    return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(c);\n";
    out << "}\n\n";
    // Named color lookup for Brush properties (maps string names to Color ARGB)
    // SVG path-data parser: XAML's Path.Data type converter. Emits a real
    // Microsoft.UI.Xaml.Media.PathGeometry (Figures built from the "M/L/C/Z ...").
    // PathGeometry is activatable; Geometry is its projected base wrapper, and the
    // converting template ctor on projected classes flows it into Geometry-typed
    // setters (PathIcon.Data). Uses get_/put_ accessors + raw IVector::Append
    // (slot 13) because the projection emits no property-syntax wrappers for
    // these media types. Point is a plain struct (winrt::Windows::Foundation).
    out << "static winrt::Microsoft::UI::Xaml::Media::PathGeometry parse_svg_path(char const* d) {\n";
    out << "    winrt::Microsoft::UI::Xaml::Media::PathGeometry geo;\n";
    out << "    auto figs = geo.get_Figures();\n";
    out << "    auto cur = winrt::Microsoft::UI::Xaml::Media::PathFigure();\n";
    out << "    bool in_fig = false;\n";
    out << "    double cx = 0, cy = 0; double cmd_x = 0, cmd_y = 0;\n";
    out << "    double prev_ctrl_x = 0, prev_ctrl_y = 0;\n";
    out << "    char last = 0; char prev_cmd = 0;\n";
    out << "    auto next_num = [&](double& out_v) -> bool {\n";
    out << "        while (*d == ' ' || *d == ',' || *d == '\\t' || *d == '\\r' || *d == '\\n') ++d;\n";
    out << "        if (!*d) return false;\n";
    out << "        char* e = nullptr;\n";
    out << "        out_v = std::strtod(d, &e);\n";
    out << "        if (e == d) return false;\n";
    out << "        d = e;\n";
    out << "        return true;\n";
    out << "    };\n";
    out << "    auto pt = [](double x, double y) { return winrt::Windows::Foundation::Point{float(x), float(y)}; };\n";
    out << "    auto fig_append = [&](winrt::Microsoft::UI::Xaml::Media::PathFigure const& f) { void* col = figs.vector_abi(); auto** vt = *reinterpret_cast<void***>(col); reinterpret_cast<void(__stdcall*)(void*, void*)>(vt[13])(col, f.m_unknown.get()); winrt::IUnknown::release_raw(col); };\n";
    out << "    auto seg_append = [&](void* seg) { auto segs = cur.get_Segments(); void* col = segs.vector_abi(); auto** vt = *reinterpret_cast<void***>(col); reinterpret_cast<void(__stdcall*)(void*, void*)>(vt[13])(col, seg); winrt::IUnknown::release_raw(col); };\n";
    out << "    auto flush_fig = [&]() { if (in_fig) { fig_append(cur); in_fig = false; } };\n";
    out << "    auto add_line = [&](double x, double y) { auto seg = winrt::Microsoft::UI::Xaml::Media::LineSegment(); seg.put_Point(pt(x, y)); seg_append(seg.m_unknown.get()); };\n";
    out << "    auto add_cubic = [&](double x1, double y1, double x2, double y2, double x, double y) { auto seg = winrt::Microsoft::UI::Xaml::Media::BezierSegment(); seg.put_Point1(pt(x1, y1)); seg.put_Point2(pt(x2, y2)); seg.put_Point3(pt(x, y)); seg_append(seg.m_unknown.get()); };\n";
    out << "    auto add_quad = [&](double x1, double y1, double x, double y) { auto seg = winrt::Microsoft::UI::Xaml::Media::QuadraticBezierSegment(); seg.put_Point1(pt(x1, y1)); seg.put_Point2(pt(x, y)); seg_append(seg.m_unknown.get()); };\n";
    out << "    auto add_arc = [&](double rx, double ry, double rot, double large, double sweep, double x, double y) { auto seg = winrt::Microsoft::UI::Xaml::Media::ArcSegment(); seg.put_Point(pt(x, y)); seg.put_Size(winrt::Windows::Foundation::Size{float(std::abs(rx)), float(std::abs(ry))}); seg.put_RotationAngle(rot); seg.put_IsLargeArc(large != 0); seg.put_SweepDirection(sweep != 0 ? winrt::Microsoft::UI::Xaml::Media::SweepDirection::Clockwise : winrt::Microsoft::UI::Xaml::Media::SweepDirection::Counterclockwise); seg_append(seg.m_unknown.get()); };\n";
    out << "    for (; *d; ) {\n";
    out << "        while (*d == ' ' || *d == ',' || *d == '\\t' || *d == '\\r' || *d == '\\n') ++d;\n";
    out << "        if (!*d) break;\n";
    out << "        char c = *d;\n";
    out << "        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { prev_cmd = last; last = c; ++d; }\n";
    out << "        else if (c != '-' && c != '+' && c != '.' && (c < '0' || c > '9')) { ++d; continue; }\n";
    out << "        switch (last) {\n";
    out << "        case 'M': case 'm': {\n";
    out << "            double x, y;\n";
    out << "            if (!next_num(x) || !next_num(y)) { last = 0; break; }\n";
    out << "            if (last == 'm') { x += cx; y += cy; }\n";
    out << "            flush_fig();\n";
    out << "            cur = winrt::Microsoft::UI::Xaml::Media::PathFigure();\n";
    out << "            cur.put_StartPoint(pt(x, y));\n";
    out << "            in_fig = true; cx = cmd_x = x; cy = cmd_y = y;\n";
    out << "            prev_cmd = last; last = (last == 'm') ? 'l' : 'L';\n";
    out << "            break; }\n";
    out << "        case 'L': case 'l': {\n";
    out << "            double x, y;\n";
    out << "            if (!next_num(x) || !next_num(y)) { last = 0; break; }\n";
    out << "            if (last == 'l') { x += cx; y += cy; }\n";
    out << "            if (!in_fig) { cur = winrt::Microsoft::UI::Xaml::Media::PathFigure(); cur.put_StartPoint(pt(cx, cy)); in_fig = true; }\n";
    out << "            add_line(x, y); cx = x; cy = y; prev_cmd = last;\n";
    out << "            break; }\n";
    out << "        case 'H': case 'h': {\n";
    out << "            double x;\n";
    out << "            if (!next_num(x)) { last = 0; break; }\n";
    out << "            if (last == 'h') x += cx;\n";
    out << "            if (!in_fig) { cur = winrt::Microsoft::UI::Xaml::Media::PathFigure(); cur.put_StartPoint(pt(cx, cy)); in_fig = true; }\n";
    out << "            add_line(x, cy); cx = x; prev_cmd = last;\n";
    out << "            break; }\n";
    out << "        case 'V': case 'v': {\n";
    out << "            double y;\n";
    out << "            if (!next_num(y)) { last = 0; break; }\n";
    out << "            if (last == 'v') y += cy;\n";
    out << "            if (!in_fig) { cur = winrt::Microsoft::UI::Xaml::Media::PathFigure(); cur.put_StartPoint(pt(cx, cy)); in_fig = true; }\n";
    out << "            add_line(cx, y); cy = y; prev_cmd = last;\n";
    out << "            break; }\n";
    out << "        case 'C': case 'c': {\n";
    out << "            double x1, y1, x2, y2, x, y;\n";
    out << "            if (!next_num(x1) || !next_num(y1) || !next_num(x2) || !next_num(y2) || !next_num(x) || !next_num(y)) { last = 0; break; }\n";
    out << "            if (last == 'c') { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }\n";
    out << "            if (!in_fig) { cur = winrt::Microsoft::UI::Xaml::Media::PathFigure(); cur.put_StartPoint(pt(cx, cy)); in_fig = true; }\n";
    out << "            add_cubic(x1, y1, x2, y2, x, y);\n";
    out << "            prev_ctrl_x = x2; prev_ctrl_y = y2; cx = x; cy = y; prev_cmd = last;\n";
    out << "            break; }\n";
    out << "        case 'S': case 's': {\n";
    out << "            double x2, y2, x, y;\n";
    out << "            if (!next_num(x2) || !next_num(y2) || !next_num(x) || !next_num(y)) { last = 0; break; }\n";
    out << "            double x1 = (prev_cmd == 'C' || prev_cmd == 'c' || prev_cmd == 'S' || prev_cmd == 's') ? (2 * cx - prev_ctrl_x) : cx;\n";
    out << "            double y1 = (prev_cmd == 'C' || prev_cmd == 'c' || prev_cmd == 'S' || prev_cmd == 's') ? (2 * cy - prev_ctrl_y) : cy;\n";
    out << "            if (last == 's') { x2 += cx; y2 += cy; x += cx; y += cy; }\n";
    out << "            if (!in_fig) { cur = winrt::Microsoft::UI::Xaml::Media::PathFigure(); cur.put_StartPoint(pt(cx, cy)); in_fig = true; }\n";
    out << "            add_cubic(x1, y1, x2, y2, x, y);\n";
    out << "            prev_ctrl_x = x2; prev_ctrl_y = y2; cx = x; cy = y; prev_cmd = last;\n";
    out << "            break; }\n";
    out << "        case 'Q': case 'q': {\n";
    out << "            double x1, y1, x, y;\n";
    out << "            if (!next_num(x1) || !next_num(y1) || !next_num(x) || !next_num(y)) { last = 0; break; }\n";
    out << "            if (last == 'q') { x1 += cx; y1 += cy; x += cx; y += cy; }\n";
    out << "            if (!in_fig) { cur = winrt::Microsoft::UI::Xaml::Media::PathFigure(); cur.put_StartPoint(pt(cx, cy)); in_fig = true; }\n";
    out << "            add_quad(x1, y1, x, y);\n";
    out << "            prev_ctrl_x = x1; prev_ctrl_y = y1; cx = x; cy = y; prev_cmd = last;\n";
    out << "            break; }\n";
    out << "        case 'T': case 't': {\n";
    out << "            double x, y;\n";
    out << "            if (!next_num(x) || !next_num(y)) { last = 0; break; }\n";
    out << "            double x1 = (prev_cmd == 'Q' || prev_cmd == 'q' || prev_cmd == 'T' || prev_cmd == 't') ? (2 * cx - prev_ctrl_x) : cx;\n";
    out << "            double y1 = (prev_cmd == 'Q' || prev_cmd == 'q' || prev_cmd == 'T' || prev_cmd == 't') ? (2 * cy - prev_ctrl_y) : cy;\n";
    out << "            if (last == 't') { x += cx; y += cy; }\n";
    out << "            if (!in_fig) { cur = winrt::Microsoft::UI::Xaml::Media::PathFigure(); cur.put_StartPoint(pt(cx, cy)); in_fig = true; }\n";
    out << "            add_quad(x1, y1, x, y);\n";
    out << "            prev_ctrl_x = x1; prev_ctrl_y = y1; cx = x; cy = y; prev_cmd = last;\n";
    out << "            break; }\n";
    out << "        case 'A': case 'a': {\n";
    out << "            double rx, ry, rot, large, sweep, x, y;\n";
    out << "            if (!next_num(rx) || !next_num(ry) || !next_num(rot) || !next_num(large) || !next_num(sweep) || !next_num(x) || !next_num(y)) { last = 0; break; }\n";
    out << "            if (last == 'a') { x += cx; y += cy; }\n";
    out << "            if (!in_fig) { cur = winrt::Microsoft::UI::Xaml::Media::PathFigure(); cur.put_StartPoint(pt(cx, cy)); in_fig = true; }\n";
    out << "            add_arc(rx, ry, rot, large, sweep, x, y);\n";
    out << "            cx = x; cy = y; prev_cmd = last;\n";
    out << "            break; }\n";
    out << "        case 'Z': case 'z':\n";
    out << "            if (in_fig) { cur.put_IsClosed(true); flush_fig(); }\n";
    out << "            cx = cmd_x; cy = cmd_y;\n";
    out << "            last = 0; prev_cmd = 'z';\n";
    out << "            break;\n";
    out << "        default:\n";
    out << "            ++d;\n";
    out << "            break;\n";
    out << "        }\n";
    out << "    }\n";
    out << "    flush_fig();\n";
    out << "    return geo;\n";
    out << "}\n\n";
    out << "static winrt::Microsoft::UI::Xaml::Media::Geometry geometry_lookup_th(wchar_t const* key) {\n";
    out << "    if (key && std::wstring_view(key) == L\"GitHubIconPath\") {\n";
    out << "        const char* gh_svg = \"M48.854 0C21.839 0 0 22 0 49.217c0 21.756 13.993 40.172 33.405 46.69 2.427.49 3.316-1.059 3.316-2.362 0-1.141-.08-5.052-.08-9.127-13.59 2.934-16.42-5.867-16.42-5.867-2.184-5.704-5.42-7.17-5.42-7.17-4.448-3.015.324-3.015.324-3.015 4.934.326 7.523 5.052 7.523 5.052 4.367 7.496 11.404 5.378 14.235 4.074.404-3.178 1.699-5.378 3.074-6.6-10.839-1.141-22.243-5.378-22.243-24.283 0-5.378 1.94-9.778 5.014-13.2-.485-1.222-2.184-6.275.486-13.038 0 0 4.125-1.304 13.426 5.052a46.97 46.97 0 0 1 12.214-1.63c4.125 0 8.33.571 12.213 1.63 9.302-6.356 13.427-5.052 13.427-5.052 2.67 6.763.97 11.816.485 13.038 3.155 3.422 5.015 7.822 5.015 13.2 0 18.905-11.404 23.06-22.324 24.283 1.78 1.548 3.316 4.481 3.316 9.126 0 6.6-.08 11.897-.08 13.526 0 1.304.89 2.853 3.316 2.364 19.412-6.52 33.405-24.935 33.405-46.691C97.707 22 75.788 0 48.854 0z\";\n";
    out << "        auto pg = parse_svg_path(gh_svg);\n";
    out << "        if (pg.get_abi()) return winrt::Microsoft::UI::Xaml::Media::Geometry::wrap_borrowed(pg.m_unknown.get());\n";
    out << "    }\n";
    out << "    winrt::IInspectable v = theme_lookup_th(key, winrt::IInspectable{});\n";
    out << "    if (!v) return winrt::Microsoft::UI::Xaml::Media::Geometry::from_abi(nullptr);\n";
    out << "    void* geom_raw = nullptr;\n";
    out << "    if (winrt::IUnknown::qi_raw(v.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Media::Geometry>, &geom_raw) >= 0 && geom_raw) {\n";
    out << "        return winrt::Microsoft::UI::Xaml::Media::Geometry::from_abi(geom_raw);\n";
    out << "    }\n";
    out << "    winrt::hstring hs = winrt::unbox_value_or<winrt::hstring>(v, L\"\");\n";
    out << "    if (!hs.empty()) {\n";
    out << "        std::string s;\n";
    out << "        for (wchar_t ch : std::wstring_view(hs.c_str())) s += (ch < 128) ? static_cast<char>(ch) : ' ';\n";
    out << "        auto pg = parse_svg_path(s.c_str());\n";
    out << "        if (pg.get_abi()) return winrt::Microsoft::UI::Xaml::Media::Geometry::wrap_borrowed(pg.m_unknown.get());\n";
    out << "    }\n";
    out << "    return winrt::Microsoft::UI::Xaml::Media::Geometry::from_abi(nullptr);\n";
    out << "}\n\n";
    out << "static winrt::Windows::UI::Color named_color(char const* name) {\n";
    out << "    struct { char const* n; std::uint8_t r,g,b; } table[] = {\n";
    out << "        {\"Black\",0,0,0},{\"White\",255,255,255},{\"Red\",255,0,0},{\"Green\",0,128,0},\n";
    out << "        {\"Blue\",0,0,255},{\"Yellow\",255,255,0},{\"Orange\",255,165,0},{\"Gray\",128,128,128},\n";
    out << "        {\"Cyan\",0,255,255},{\"Magenta\",255,0,255},{\"Brown\",139,69,19},{\"Transparent\",0,0,0},\n";
    out << "        {\"DarkGray\",169,169,169},{\"LightGray\",211,211,211},{\"DarkRed\",139,0,0},\n";
    out << "        {\"DarkGreen\",0,100,0},{\"DarkBlue\",0,0,139},{\"SteelBlue\",70,130,180},\n";
    out << "        {\"DarkGoldenrod\",184,134,11},{\"Gold\",255,215,0},{\"Indigo\",75,0,130},\n";
    out << "        {\"Violet\",238,130,238},{\"Purple\",128,0,128},{\"Navy\",0,0,128},\n";
    out << "        {\"Teal\",0,128,128},{\"Olive\",128,128,0},{\"Maroon\",128,0,0},\n";
    out << "        {\"Silver\",192,192,192},{\"Khaki\",240,230,140},{\"Coral\",255,127,80},\n";
    out << "        {\"Tomato\",255,99,71},{\"Salmon\",250,128,114},{\"Sienna\",160,82,45},\n";
    out << "        {\"Wheat\",245,222,179},{\"Ivory\",255,255,240},{\"Linen\",250,240,230},\n";
    out << "        {\"Aquamarine\",127,255,212},{\"Turquoise\",64,224,208},{\"Orchid\",218,112,214},\n";
    out << "        {\"HotPink\",255,105,180},{\"Crimson\",220,20,60},{\"LimeGreen\",50,205,50},\n";
    out << "        {\"ForestGreen\",34,139,34},{\"SpringGreen\",0,255,127},{\"DodgerBlue\",30,144,255},\n";
    out << "        {\"CornflowerBlue\",100,149,237},{\"RoyalBlue\",65,105,225},{\"SlateBlue\",106,90,205},\n";
    out << "        {\"DeepSkyBlue\",0,191,255},{\"AliceBlue\",240,248,255},{\"Honeydew\",240,255,240},\n";
    out << "        {\"MintCream\",245,255,250},{\"LavenderBlush\",255,240,245},{\"Seashell\",255,245,238},\n";
    out << "        {\"Snow\",255,250,250},{\"FireBrick\",178,34,34},{\"DarkSlateGray\",47,79,79},\n";
    out << "        {\"DimGray\",105,105,105},{\"MediumSlateBlue\",123,104,238},{\"DarkOrange\",255,140,0},\n";
    out << "        {\"Chocolate\",210,105,30},{\"Peru\",205,133,63},{\"Tan\",210,180,140},\n";
    out << "        {\"SkyBlue\",135,206,235},{\"LightSkyBlue\",135,206,250},{\"MidnightBlue\",25,25,112},\n";
    out << "        {\"CadetBlue\",95,158,160},{\"PowderBlue\",176,224,230},{\"LightBlue\",173,216,230},\n";
    out << "        {\"PaleTurquoise\",175,238,238},{\"MediumTurquoise\",72,209,204},{\"DarkTurquoise\",0,206,209},\n";
    out << "        {\"MediumPurple\",147,112,219},{\"BlueViolet\",138,43,226},{\"DarkOrchid\",153,50,204},\n";
    out << "        {\"MediumVioletRed\",199,21,133},{\"DeepPink\",255,20,147},{\"PaleVioletRed\",219,112,147},\n";
    out << "        {\"LawnGreen\",124,252,0},{\"Chartreuse\",127,255,0},{\"GreenYellow\",173,255,47},\n";
    out << "        {\"DarkSeaGreen\",143,188,143},{\"SeaGreen\",46,139,87},{\"MediumSeaGreen\",60,179,113},\n";
    out << "        {\"DarkOliveGreen\",85,107,47},{\"OliveDrab\",107,142,35},{\"DarkKhaki\",189,183,107},\n";
    out << "        {\"RosyBrown\",188,143,143},{\"IndianRed\",205,92,92},{\"SaddleBrown\",139,69,19},\n";
    out << "        {\"SandyBrown\",244,164,96},{\"BurlyWood\",222,184,135},{\"Wheat\",245,222,179},\n";
    out << "        {\"AntiqueWhite\",250,235,215},{\"Beige\",245,245,220},{\"Linen\",250,240,230},\n";
    out << "        {\"FloralWhite\",255,250,240},{\"Lavender\",230,230,250},{\"MistyRose\",255,228,225}\n";
    out << "    };\n";
    out << "    for (auto& t : table) { if (std::string_view(name) == t.n) return {255,t.r,t.g,t.b}; }\n";
    out << "    return {255,0,0,0}; // fallback black\n";
    out << "}\n\n";
    // Unprojected type stubs — types in WinUI3 XAML but NOT projected by cppwinrt.
    // Only add stubs for types genuinely missing from the WinRT projection.
    // DO NOT add stubs for types already in winrt.Microsoft.UI.Xaml.P1 — that shadows them.
    // Each concrete stub activates its REAL runtime class (Shapes.* are live
    // types in Microsoft.UI.Xaml.dll) — inheriting FrameworkElement's ctor
    // would try to activate the abstract base and fail-fast (E_NOINTERFACE).
    auto emit_shape_ctor = [&out](char const* cls)
    {
        out << "    " << cls << "()\n    {\n";
        out << "        void* inst = winrt::impl::create_instance(L\"Microsoft.UI.Xaml.Shapes." << cls << "\");\n";
        out << "        void* unk = nullptr;\n";
        out << "        winrt::check_hresult(winrt::impl::query_interface(inst, winrt::iid_of<winrt::IInspectable>, &unk));\n";
        out << "        auto* ivt = *reinterpret_cast<void* const**>(inst);\n";
        out << "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(ivt[2])(inst);\n";
        out << "        m_unknown = winrt::com_ptr<void>::attach(unk);\n";
        out << "    }\n";
    };
    out << "namespace winrt::Microsoft::UI::Xaml::Shapes {\n";
    out << "struct Shape : winrt::Microsoft::UI::Xaml::FrameworkElement {\n";
    out << "    using FrameworkElement::FrameworkElement;\n";
    out << "    Shape() : FrameworkElement(typename FrameworkElement::wrap_tag_t{}) {} // null wrapper: derived stubs activate their own class\n";
    out << "    winrt::Microsoft::UI::Xaml::Media::Brush Fill() { return {}; }\n";
    out << "    void Fill(winrt::Microsoft::UI::Xaml::Media::Brush const&) {}\n";
    out << "    winrt::Microsoft::UI::Xaml::Media::Brush Stroke() { return {}; }\n";
    out << "    void Stroke(winrt::Microsoft::UI::Xaml::Media::Brush const&) {}\n";
    out << "    double StrokeThickness() { return 0; }\n";
    out << "    void StrokeThickness(double) {}\n";
    out << "    double Opacity() { return 1.0; }\n";
    out << "    void Opacity(double) {}\n";
    out << "    void Width(double) {}\n";
    out << "    void Height(double) {}\n";
    out << "    void Margin(winrt::Microsoft::UI::Xaml::Thickness const&) {}\n";
    out << "    void Padding(winrt::Microsoft::UI::Xaml::Thickness const&) {}\n";
    out << "};\n";
    out << "struct Ellipse : Shape { using Shape::Shape;\n"; emit_shape_ctor("Ellipse"); out << "};\n";
    out << "struct Rectangle : Shape {\n";
    out << "    using Shape::Shape;\n";
    emit_shape_ctor("Rectangle");
    out << "    double RadiusX() { return 0; }\n";
    out << "    void RadiusX(double) {}\n";
    out << "    double RadiusY() { return 0; }\n";
    out << "    void RadiusY(double) {}\n";
    out << "};\n";
    out << "struct Line : Shape {\n";
    out << "    using Shape::Shape;\n";
    emit_shape_ctor("Line");
    out << "    double X1() { return 0; } void X1(double) {}\n";
    out << "    double Y1() { return 0; } void Y1(double) {}\n";
    out << "    double X2() { return 0; } void X2(double) {}\n";
    out << "    double Y2() { return 0; } void Y2(double) {}\n";
    out << "};\n";
    out << "struct Polygon : Shape { using Shape::Shape;\n"; emit_shape_ctor("Polygon"); out << "};\n";
    out << "struct Polyline : Shape { using Shape::Shape;\n"; emit_shape_ctor("Polyline"); out << "};\n";
    out << "struct Path : Shape {\n";
    out << "    using Shape::Shape;\n";
    emit_shape_ctor("Path");
    out << "    void Data(winrt::IInspectable const&) {}\n";
    out << "};\n";
    out << "} // namespace winrt::Microsoft::UI::Xaml::Shapes\n\n";
    // Windows.Foundation.Uri — CppWinC already projects it into the
    // winrt.Windows.Foundation module; a local stub would shadow that
    // real entity and split every cross-module symbol passing a Uri
    // (LNK2019 $$_A vs $$_B). Stub only for SDK-less runs.
    if (!have_windows_sdk) {
    out << "namespace winrt::Windows::Foundation {\n";
    out << "struct Uri : winrt::IInspectable {\n";
    out << "    Uri() {}\n";
    out << "    Uri(winrt::hstring const&) {}\n";
    out << "    winrt::hstring AbsoluteUri() { return {}; }\n";
    out << "    winrt::hstring RawUri() { return {}; }\n";
    out << "};\n";
    out << "} // namespace winrt::Windows::Foundation\n\n";
    }
    // Media types: CppWinC projects BitmapImage into Media::Imaging (P1),
    // while XAML payloads name it Media::BitmapImage. With the SDK metadata
    // present, emit a namespace alias (an alias is not a new entity — no
    // ODR/symbol split); without it, fall back to a local stub.
    if (have_windows_sdk) {
    out << "namespace winrt::Microsoft::UI::Xaml::Media {\n";
    out << "using BitmapImage = winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage;\n";
    out << "} // namespace winrt::Microsoft::UI::Xaml::Media\n\n";
    }
    else {
    out << "namespace winrt::Microsoft::UI::Xaml::Media {\n";
    out << "struct BitmapImage : ImageSource {\n";
    out << "    using ImageSource::ImageSource;\n";
    out << "    BitmapImage() {}\n";
    out << "    BitmapImage(winrt::Windows::Foundation::Uri const&) {}\n";
    out << "    void UriSource(winrt::Windows::Foundation::Uri const&) {}\n";
    out << "};\n";
    out << "} // namespace winrt::Microsoft::UI::Xaml::Media\n\n";
    }
}
