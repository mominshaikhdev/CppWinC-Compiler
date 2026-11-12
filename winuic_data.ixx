// winuic_data.ixx — module winuic_data.
// WinUI JSON parser, catalog, expressions, templates, controls, and views.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <sstream>
#include <memory>

export module winuic_data;

export namespace winuic
{
// JSON & Layout AST

struct json_value;

struct json_object
{
    std::vector<std::pair<std::string, json_value>> items;
    json_value const* find(std::string_view key) const;
};

struct json_value
{
    enum kind_t { null_k, str, num, bool_k, arr, obj } kind = null_k;
    std::string   s;
    double        n = 0;
    bool          b = false;
    std::vector<json_value> a;
    json_object   o;

    bool is_null()  const { return kind == null_k; }
    bool is_str()   const { return kind == str; }
    bool is_num()   const { return kind == num; }
    bool is_bool()  const { return kind == bool_k; }
    bool is_arr()   const { return kind == arr; }
    bool is_obj()   const { return kind == obj; }

    json_value const* find(std::string_view key) const
    {
        if (kind == obj) return o.find(key);
        return nullptr;
    }

    std::string const& str_or(std::string_view) const
    {
        static const std::string empty;
        return (kind == str) ? s : empty;
    }

    double num_or(double def) const { return (kind == num) ? n : def; }
    bool bool_or(bool def) const { return (kind == bool_k) ? b : def; }
};

inline json_value const* json_object::find(std::string_view key) const
{
    for (auto const& [k, v] : items)
        if (k == key) return &v;
    return nullptr;
}

// 2  JSON parser

class json_parser
{
    std::string_view src_;
    std::size_t      pos_ = 0;

    void skip_ws() { while (pos_ < src_.size() && (src_[pos_]==' '||src_[pos_]=='\t'||src_[pos_]=='\n'||src_[pos_]=='\r')) ++pos_; }
    char peek() { skip_ws(); return pos_ < src_.size() ? src_[pos_] : '\0'; }
    char next() { skip_ws(); return pos_ < src_.size() ? src_[pos_++] : '\0'; }
    void expect(char c) { char g = next(); if (g != c) throw std::runtime_error(std::string("JSON: expected '")+c+"'"); }

    json_value parse_value()
    {
        char c = peek();
        if (c == '"') return { json_value::str, parse_string(), 0, false, {}, {} };
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't') { pos_ += 4; return { json_value::bool_k, {}, 0, true }; }
        if (c == 'f') { pos_ += 5; return { json_value::bool_k, {}, 0, false }; }
        if (c == 'n') { pos_ += 4; return {}; }
        return parse_number();
    }

    std::string parse_string()
    {
        expect('"');
        std::string out;
        while (pos_ < src_.size())
        {
            char c = src_[pos_++];
            if (c == '"') break;
            if (c == '\\')
            {
                if (pos_ >= src_.size()) break;
                char e = src_[pos_++];
                switch (e) { case '"': out += '"'; break; case '\\': out += '\\'; break; case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break; default: out += e; break; }
            }
            else out += c;
        }
        return out;
    }

    json_value parse_number()
    {
        json_value v;
        v.kind = json_value::num;
        std::size_t start = pos_;
        if (pos_ < src_.size() && src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') { ++pos_; while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_; }
        v.n = std::strtod(std::string(src_.substr(start, pos_ - start)).c_str(), nullptr);
        return v;
    }

    json_value parse_object()
    {
        expect('{');
        json_value v; v.kind = json_value::obj;
        if (peek() == '}') { ++pos_; return v; }
        while (true) { std::string key = parse_string(); expect(':'); v.o.items.emplace_back(std::move(key), parse_value()); if (peek() != ',') break; ++pos_; }
        expect('}');
        return v;
    }

    json_value parse_array()
    {
        expect('[');
        json_value v; v.kind = json_value::arr;
        if (peek() == ']') { ++pos_; return v; }
        while (true) { v.a.push_back(parse_value()); if (peek() != ',') break; ++pos_; }
        expect(']');
        return v;
    }

public:
    json_value parse(std::string_view json) { src_ = json; pos_ = 0; return parse_value(); }
};


// 4  Layout node (parsed from JSON)

struct layout_state_var
{
    std::string name;
    std::string type;
    std::string initial;
};

struct layout_event
{
    std::string name;
    std::vector<std::string> actions;
};

struct layout_node
{
    std::string control;
    std::vector<std::pair<std::string, std::string>> props;
    std::vector<layout_event> events;
    std::vector<layout_node> children;
    std::vector<std::pair<std::string, std::string>> raw_attrs;
    std::vector<std::string> action;
    bool is_if = false;
    std::string condition;
    std::vector<layout_node> else_branch;
    bool is_foreach = false;
    std::string collection;
};

struct layout_view
{
    std::string name;
    std::string namespace_name;  // e.g. "App" — used as module prefix
    std::string base_type;
    std::vector<layout_state_var> state_vars;
    std::vector<layout_node> root;
    // code-behind logic: callback name (PascalCase) -> statement list
    std::map<std::string, std::vector<std::string>> callbacks;
    bool dark_titlebar = false;
    bool app_icon = false;
    bool dark_context_menu = false;
    bool extend_content_into_titlebar = false;
    bool has_main_window = false;
    // App-shell view: the Application-derived view also carries the main
    // window body (`export view App` with chrome directives). The module
    // emits the AppHost machinery AND a module-local window class from the
    // same body - no separate Window view exists in the project.
    bool shell_window = false;
    // Root window class as declared by the project (base type Window) - the
    // host constructs whatever the project named it, no App.xaml/MainWindow
    // hardcoded coupling.
    std::string main_window_class;
    std::string main_window_ns;
    std::string app_icon_path;
    std::string backdrop_type;
    std::string titlebar_text;
    // NavigationView tag -> page runtime class name ("ns.Page"), in menu
    // order (driver-computed from the project's own views; never hardcoded).
    // Non-empty only for window views; drives the synthesized nav handler.
    std::vector<std::pair<std::string, std::string>> nav_pages;
    // View modules this view instantiates via get_<Name>() (driver-computed
    // from node control names matching other project views). Each entry is a
    // dotted module name to import for the sibling's exported accessor.
    std::vector<std::string> component_view_imports;
    // Events other views attach to THIS view's instances (driver-aggregated).
    // The component class exposes add_<E>/_fire_<E> forwarding for these.
    std::vector<std::string> exposed_component_events;
    std::vector<std::string> extra_app_resources;
};

// 5  Parse layout JSON into layout_view

inline layout_node parse_body_node(json_value const& j)
{
    layout_node node;
    if (auto const* c = j.find("control")) node.control = c->str_or("");

    if (node.control == "If")
    {
        node.is_if = true;
        if (auto const* p = j.find("props"); p && p->is_obj())
            if (auto const* cond = p->find("condition"))
            {
                node.condition = cond->str_or("");
                if (node.condition.size() >= 2 && node.condition.front() == '"' && node.condition.back() == '"')
                    node.condition = node.condition.substr(1, node.condition.size() - 2);
            }
        if (auto const* ch = j.find("children"); ch && ch->is_arr())
            for (auto const& c : ch->a) node.children.push_back(parse_body_node(c));
        if (auto const* el = j.find("else"); el && el->is_arr())
            for (auto const& e : el->a) node.else_branch.push_back(parse_body_node(e));
        return node;
    }
    if (node.control == "ForEach")
    {
        node.is_foreach = true;
        if (auto const* p = j.find("props"); p && p->is_obj())
            if (auto const* col = p->find("collection"))
            {
                node.collection = col->str_or("");
                if (node.collection.size() >= 2 && node.collection.front() == '"' && node.collection.back() == '"')
                    node.collection = node.collection.substr(1, node.collection.size() - 2);
            }
        if (auto const* ch = j.find("children"); ch && ch->is_arr())
            for (auto const& c : ch->a) node.children.push_back(parse_body_node(c));
        return node;
    }

    if (auto const* p = j.find("props"); p && p->is_obj())
    {
        for (auto const& [k, v] : p->o.items)
        {
            std::string val;
            if (v.is_str()) val = v.s;
            else if (v.is_num()) val = (v.n == static_cast<int64_t>(v.n)) ? std::to_string(static_cast<int64_t>(v.n)) : std::to_string(v.n);
            else if (v.is_bool()) val = v.b ? "true" : "false";
            node.props.emplace_back(k, std::move(val));
        }
    }

    if (auto const* ev = j.find("events"); ev && ev->is_arr())
        for (auto const& e : ev->a)
        {
            layout_event le;
            if (auto const* n = e.find("name"))  le.name = n->str_or("");
            if (auto const* a = e.find("action"); a && a->is_arr())
                for (auto const& s : a->a) le.actions.push_back(s.str_or(""));
            if (!le.name.empty()) node.events.push_back(std::move(le));
        }

    if (auto const* ra = j.find("raw"); ra && ra->is_arr())
        for (auto const& pair : ra->a)
            if (pair.is_arr() && pair.a.size() >= 2)
                node.raw_attrs.emplace_back(pair.a[0].str_or(""), pair.a[1].str_or(""));

    if (auto const* ac = j.find("action"); ac && ac->is_arr())
        for (auto const& s : ac->a) node.action.push_back(s.str_or(""));

    if (auto const* ch = j.find("children"); ch && ch->is_arr())
        for (auto const& c : ch->a) node.children.push_back(parse_body_node(c));

    return node;
}

inline layout_view parse_layout_json(std::string_view json, std::string_view view_name)
{
    layout_view view;
    view.name = view_name;
    json_parser p;
    auto root = p.parse(json);
    if (!root.is_arr()) return view;

    for (auto const& node : root.a)
    {
        auto const* ctrl = node.find("control");
        if (ctrl && ctrl->str_or("") == "state")
        {
            if (auto const* props = node.find("props"); props && props->is_obj())
                if (auto const* vars = props->find("vars"); vars && vars->is_arr())
                    for (auto const& v : vars->a)
                    {
                        layout_state_var sv;
                        if (auto const* n = v.find("name"))  sv.name = n->str_or("");
                        if (auto const* t = v.find("type"))  sv.type = t->str_or("string");
                        if (auto const* iv = v.find("value"))
                        {
                            if (iv->is_str()) sv.initial = "\"" + iv->s + "\"";
                            else if (iv->is_num()) sv.initial = (iv->n == static_cast<int64_t>(iv->n)) ? std::to_string(static_cast<int64_t>(iv->n)) : std::to_string(iv->n);
                            else if (iv->is_bool()) sv.initial = iv->b ? "true" : "false";
                        }
                        view.state_vars.push_back(std::move(sv));
                    }
        }
        else if (ctrl && ctrl->str_or("") == "callbacks")
        {
            // code-behind logic: {"control":"callbacks","props":{"cbs":[{"name":"X","body":[...]}]}}
            if (auto const* props = node.find("props"); props && props->is_obj())
                if (auto const* cbs = props->find("cbs"); cbs && cbs->is_arr())
                    for (auto const& cb : cbs->a)
                    {
                        std::string cb_name;
                        if (auto const* n = cb.find("name")) cb_name = n->str_or("");
                        std::vector<std::string> stmts;
                        if (auto const* b = cb.find("body"); b && b->is_arr())
                            for (auto const& s : b->a) stmts.push_back(s.str_or(""));
                        if (!cb_name.empty())
                            view.callbacks[cb_name] = std::move(stmts);
                    }
        }
        else
        {
            view.root.push_back(parse_body_node(node));
        }
    }
    return view;
}

// 6  C++23 module code generation

inline std::string to_cxx_type(std::string_view json_type)
{
    if (json_type == "int")    return "int";
    if (json_type == "double") return "double";
    if (json_type == "bool")   return "bool";
    if (json_type == "string") return "winrt::hstring";
    // Control type references — map to WinRT types so property calls compile
    std::string s(json_type);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "toggleswitch")   return "winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch";
    if (s == "textbox")        return "winrt::Microsoft::UI::Xaml::Controls::TextBox";
    if (s == "textblock")      return "winrt::Microsoft::UI::Xaml::Controls::TextBlock";
    if (s == "checkbox")       return "winrt::Microsoft::UI::Xaml::Controls::CheckBox";
    if (s == "combobox")       return "winrt::Microsoft::UI::Xaml::Controls::ComboBox";
    if (s == "slider")         return "winrt::Microsoft::UI::Xaml::Controls::Slider";
    if (s == "progressbar")    return "winrt::Microsoft::UI::Xaml::Controls::ProgressBar";
    if (s == "progressring")   return "winrt::Microsoft::UI::Xaml::Controls::ProgressRing";
    if (s == "radiobutton")    return "winrt::Microsoft::UI::Xaml::Controls::RadioButton";
    if (s == "button")         return "winrt::Microsoft::UI::Xaml::Controls::Button";
    if (s == "image")          return "winrt::Microsoft::UI::Xaml::Controls::Image";
    if (s == "listview")       return "winrt::Microsoft::UI::Xaml::Controls::ListView";
    if (s == "gridview")       return "winrt::Microsoft::UI::Xaml::Controls::GridView";
    if (s == "treeview")       return "winrt::Microsoft::UI::Xaml::Controls::TreeView";
    if (s == "navigationview") return "winrt::Microsoft::UI::Xaml::Controls::NavigationView";
    if (s == "tabview")        return "winrt::Microsoft::UI::Xaml::Controls::TabView";
    if (s == "expander")       return "winrt::Microsoft::UI::Xaml::Controls::Expander";
    if (s == "infobar")        return "winrt::Microsoft::UI::Xaml::Controls::InfoBar";
    if (s == "numberbox")      return "winrt::Microsoft::UI::Xaml::Controls::NumberBox";
    if (s == "passwordbox")    return "winrt::Microsoft::UI::Xaml::Controls::PasswordBox";
    if (s == "richeditbox")    return "winrt::Microsoft::UI::Xaml::Controls::RichEditBox";
    if (s == "calendarview")   return "winrt::Microsoft::UI::Xaml::Controls::CalendarView";
    if (s == "calendardatepicker") return "winrt::Microsoft::UI::Xaml::Controls::CalendarDatePicker";
    if (s == "datepicker")     return "winrt::Microsoft::UI::Xaml::Controls::DatePicker";
    if (s == "timepicker")     return "winrt::Microsoft::UI::Xaml::Controls::TimePicker";
    if (s == "colorpicker")    return "winrt::Microsoft::UI::Xaml::Controls::ColorPicker";
    if (s == "ratingcontrol")  return "winrt::Microsoft::UI::Xaml::Controls::RatingControl";
    if (s == "pipspager")      return "winrt::Microsoft::UI::Xaml::Controls::PipsPager";
    if (s == "breadcrumbbar")  return "winrt::Microsoft::UI::Xaml::Controls::BreadcrumbBar";
    if (s == "menubar")        return "winrt::Microsoft::UI::Xaml::Controls::MenuBar";
    if (s == "swipecontrol")   return "winrt::Microsoft::UI::Xaml::Controls::SwipeControl";
    if (s == "infobadge")      return "winrt::Microsoft::UI::Xaml::Controls::InfoBadge";
    if (s == "teachingtip")    return "winrt::Microsoft::UI::Xaml::Controls::TeachingTip";
    if (s == "contentdialog")  return "winrt::Microsoft::UI::Xaml::Controls::ContentDialog";
    if (s == "flyout")         return "winrt::Microsoft::UI::Xaml::Controls::Flyout";
    if (s == "menuflyout")     return "winrt::Microsoft::UI::Xaml::Controls::MenuFlyout";
    if (s == "popup")          return "winrt::Microsoft::UI::Xaml::Controls::Primitives::Popup";
    if (s == "tooltip")        return "winrt::Microsoft::UI::Xaml::Controls::ToolTip";
    if (s == "scrollviewer")   return "winrt::Microsoft::UI::Xaml::Controls::ScrollViewer";
    if (s == "scrollview")     return "winrt::Microsoft::UI::Xaml::Controls::ScrollView";
    if (s == "flipview")       return "winrt::Microsoft::UI::Xaml::Controls::FlipView";
    if (s == "pivot")          return "winrt::Microsoft::UI::Xaml::Controls::Pivot";
    if (s == "itemsrepeater")  return "winrt::Microsoft::UI::Xaml::Controls::ItemsRepeater";
    if (s == "itemsview")      return "winrt::Microsoft::UI::Xaml::Controls::ItemsView";
    if (s == "commandbar")     return "winrt::Microsoft::UI::Xaml::Controls::CommandBar";
    if (s == "splitbutton")    return "winrt::Microsoft::UI::Xaml::Controls::SplitButton";
    if (s == "collectionviewsource") return "winrt::Microsoft::UI::Xaml::Data::CollectionViewSource";
    if (s == "xamluicommand")   return "winrt::Microsoft::UI::Xaml::Input::XamlUICommand";
    if (s == "dropdownbutton") return "winrt::Microsoft::UI::Xaml::Controls::DropDownButton";
    if (s == "personpicture")  return "winrt::Microsoft::UI::Xaml::Controls::PersonPicture";
    if (s == "mediaplayerelement") return "winrt::Microsoft::UI::Xaml::Controls::MediaPlayerElement";
    if (s == "webview2")       return "winrt::Microsoft::UI::Xaml::Controls::WebView2";
    if (s == "stackpanel")     return "winrt::Microsoft::UI::Xaml::Controls::StackPanel";
    if (s == "grid")           return "winrt::Microsoft::UI::Xaml::Controls::Grid";
    if (s == "canvas")         return "winrt::Microsoft::UI::Xaml::Controls::Canvas";
    if (s == "border")         return "winrt::Microsoft::UI::Xaml::Controls::Border";
    if (s == "frame")          return "winrt::Microsoft::UI::Xaml::Controls::Frame";
    if (s == "scrollbar")      return "winrt::Microsoft::UI::Xaml::Controls::Primitives::ScrollBar";
    if (s == "row" || s == "column" || s == "stack") return "winrt::Microsoft::UI::Xaml::Controls::StackPanel";
    if (s == "card")           return "winrt::Microsoft::UI::Xaml::Controls::Border";
    if (s == "toggle")         return "winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch";
    if (s == "spacer")         return "winrt::Microsoft::UI::Xaml::Shapes::Rectangle";
    if (s == "divider")        return "winrt::Microsoft::UI::Xaml::Shapes::Rectangle";
    if (s == "input")          return "winrt::Microsoft::UI::Xaml::Controls::TextBox";
    // Fallback: a capitalized name is a control/element type reference
    // (CopyButton, InlineColorPicker, BitmapImage, CollectionViewSource, ...).
    // The module emits `using namespace` directives for every projected and
    // component namespace before the view class, and element code already
    // constructs these unqualified (auto X = CopyButton();). Qualifying with
    // winrt::Microsoft::UI::Xaml::Controls is wrong for app controls and for
    // Media/Data types - emit the bare name so ordinary lookup resolves it
    // exactly like the element-construction path does.
    if (s == "any" || s == "object") return "winrt::IInspectable";
    if (s == "any[]" || s == "object[]") return "winrt::Windows::Foundation::Collections::IObservableVector<winrt::IInspectable>";
    if (!json_type.empty() && std::isupper(static_cast<unsigned char>(json_type[0])))
        return std::string(json_type);
    return "winrt::hstring";
}

// Sanitize a string for use as a C++ identifier (no ::, spaces, etc.)
inline std::string sanitize_id(std::string s)
{
    for (auto& c : s) if (c == ':' || c == '.' || c == ' ') c = '_';
    return s;
}



// Expressions & Property Resolution

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
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 128) {
            switch (c) {
            case '\\': r += "\\\\"; break;
            case '"':  r += "\\\""; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:   r += static_cast<char>(c); break;
            }
            ++i;
        } else {
            uint32_t cp = 0;
            int len = 0;
            if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F; len = 2;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F; len = 3;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07; len = 4;
            } else {
                ++i;
                continue;
            }
            bool valid = true;
            for (int k = 1; k < len; ++k) {
                if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
            }
            if (valid) {
                i += len;
                if (cp <= 0xFFFF) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "\\u%04X", cp);
                    r += buf;
                } else {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "\\U%08X", cp);
                    r += buf;
                }
            } else {
                ++i;
            }
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
    if (already_wstr) return "winrt::box_value(winrt::hstring(" + val + "))";
    bool already_str = (val.size() >= 2 && val.front() == '"' && val.back() == '"');
    if (already_str) return "winrt::box_value(winrt::hstring(L" + val + "))";
    if (val.find("(") != std::string::npos && val.find(")") != std::string::npos) return val;
    if (val.find("box_value") != std::string::npos) return val;
    if (val.find("make_solid_brush") != std::string::npos) return val;
    if (val.find("Brush::from_abi") != std::string::npos) return val;
    if (val.find("from_abi") != std::string::npos) return val;
    bool is_num = !val.empty() && (std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-');
    if (is_num) return "winrt::box_value(" + val + ")";
    return "winrt::box_value(winrt::hstring(L\"" + val + "\"))";
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
    if (already_wstr) return "winrt::box_value(winrt::hstring(" + val + "))";
    bool already_str = (val.size() >= 2 && val.front() == '"' && val.back() == '"');
    if (already_str) return "winrt::box_value(winrt::hstring(L" + val + "))";
    if (val.find("(") != std::string::npos && val.find(")") != std::string::npos) return val;
    if (val.find("box_value") != std::string::npos) return val;
    if (val.find("make_solid_brush") != std::string::npos) return val;
    if (val.find("Brush::from_abi") != std::string::npos) return val;
    if (val.find("from_abi") != std::string::npos) return val;
    bool is_num = !val.empty() && (std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-');
    if (is_num) return "winrt::box_value(" + val + ")";
    return "winrt::box_value(winrt::hstring(L\"" + val + "\"))";
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
        if (clean.find("SymbolThemeFontFamily") != std::string::npos)
            clean = "Segoe Fluent Icons, Segoe MDL2 Assets";
        else if (clean.find("ContentControlThemeFontFamily") != std::string::npos)
            clean = "Segoe UI";
        else if (clean.find("Resource ") != std::string::npos)
        {
            auto ks = clean.find("Resource ") + 9;
            auto ke = clean.find('}', ks);
            if (ke != std::string::npos) clean = clean.substr(ks, ke - ks);
            if (clean.find("Symbol") != std::string::npos) clean = "Segoe Fluent Icons, Segoe MDL2 Assets";
            else clean = "Segoe UI";
        }
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
            return "winrt::box_value(winrt::hstring(L\"" + val + "\"))";
        }
    }
    // Tag/SelectedItem: IInspectable box_value wrapper
    if (pk == "tag" || pk == "Tag" || pk == "selecteditem" || pk == "SelectedItem")
    {
        if (!val.empty() && val != "nullptr")
        {
            bool is_num = std::isdigit(static_cast<unsigned char>(val.front())) || val.front() == '-';
            if (is_num) return "winrt::box_value(" + val + ")";
            return "winrt::box_value(winrt::hstring(L\"" + val + "\"))";
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


// Control Catalog & Component Stubs

struct platform_mapping
{
    std::string platform_type;
    std::string platform_namespace;
    std::vector<std::pair<std::string, std::string>> fixed_properties;
};

struct catalog_entry
{
    std::string abstract_name;
    std::string category;
    bool        accepts_children = false;
    bool        accepts_text_content = false;
    std::string description;
    platform_mapping platform;
    // Composition kind determines how children are attached:
    //   "panel"       → Children().Append(child)
    //   "content"     → Content(child)  (single child)
    //   "items"       → Items().Append(child)  or MenuItems/TabItems
    //   "items_panel" → MenuItems().Append(child)
    //   empty         → heuristic based on accepts_children
    std::string composition_kind;
    // Control type: "xaml" = XAML TemplatedControl (needs ResourceDictionary merge),
    //               "cpp"  = pure C++/WinRT component (direct instantiation).
    // The .winmd is projection-agnostic; the downstream projection decides
    // whether to emit ResourceDictionary merge (xaml) or direct activation (cpp).
    std::string control_type;           // "xaml" | "cpp"
    std::string template_resource_key;  // x:Key in ResourceDictionary (xaml only)
    std::string resource_dictionary;    // relative path to .xaml in package (xaml only)

    // Registry-declared control surface (extend / modify / create): the
    // property types and event names a registry JSON declares, normalized to
    // the same vocabulary the metadata-driven rules consume. Lets an
    // extended, overridden or brand-new control reach R6/R41/R36 exactly
    // like a winmd-declared type does - no compiler rebuild.
    std::vector<std::pair<std::string, std::string>> prop_types; // property -> canonical type name
    std::vector<std::string> event_names;                        // event names (XAML spelling)
};

// Canonical fixup-rule type name for a registry property declaration.
// Returns "" when the registry carries no type knowledge the rules use
// (unknown behaves exactly like metadata-absent: not numeric, no wrap).
inline std::string canon_prop_type(std::string_view type_s, std::string_view transform_s)
{
    auto lo = [](std::string_view s) {
        std::string o(s);
        for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return o;
    };
    std::string const t = lo(type_s), f = lo(transform_s);
    if (t == "double") return "double";
    if (t == "float") return "float";
    if (t == "int" || t == "integer") return "int32_t";
    if (t == "bool" || t == "boolean") return "bool";
    if (t == "gridlength") return "GridLength";
    if (t == "thickness") return "Thickness";
    if (t == "fontweight") return "FontWeight";
    if (f == "double") return "double";
    if (f == "int") return "int32_t";
    if (f == "bool" || f == "nullable_bool") return "bool";
    if (f == "gridlength") return "GridLength";
    if (f == "thickness") return "Thickness";
    if (f == "font_weight") return "FontWeight";
    return "";
}

class attribute_catalog
{
    std::unordered_map<std::string, catalog_entry> entries_;
    std::vector<std::string> resource_dictionaries_;
public:
    void load_from_json(std::string_view json)
    {
        if (json.empty()) return;
        json_parser p;
        auto root = p.parse(json);
        auto const* res_dicts = root.find("resource_dictionaries");
        if (res_dicts && res_dicts->is_arr())
        {
            for (auto const& rd : res_dicts->a)
            {
                if (rd.is_str() && !rd.s.empty())
                    resource_dictionaries_.push_back(rd.s);
            }
        }
        auto const* controls = root.find("controls");
        if (!controls || !controls->is_arr()) return;
        for (auto const& c : controls->a)
        {
            catalog_entry e;
            if (auto const* v = c.find("abstract_name"))  e.abstract_name = v->str_or("");
            if (auto const* v = c.find("category"))       e.category = v->str_or("");
            if (auto const* v = c.find("accepts_children"))    e.accepts_children = v->bool_or(false);
            if (auto const* v = c.find("accepts_text_content")) e.accepts_text_content = v->bool_or(false);
            if (auto const* v = c.find("platform_type"))       e.platform.platform_type = v->str_or("");
            if (auto const* v = c.find("platform_namespace"))  e.platform.platform_namespace = v->str_or("");
            if (auto const* v = c.find("composition_kind"))   e.composition_kind = v->str_or("");
            if (auto const* v = c.find("type"))                 e.control_type = v->str_or("cpp");
            if (auto const* v = c.find("template_resource_key")) e.template_resource_key = v->str_or("");            if (auto const* v = c.find("resource_dictionary"))    e.resource_dictionary = v->str_or("");
            // Registry-declared properties/events (extend/modify/create):
            // normalized into the same vocabulary the rules consume.
            if (auto const* pr = c.find("properties"); pr && pr->is_arr())
            {
                for (auto const& pv : pr->a)
                {
                    std::string pn, pt, tr;
                    if (auto const* v = pv.find("name"))      pn = v->str_or("");
                    if (auto const* v = pv.find("type"))      pt = v->str_or("");
                    if (auto const* v = pv.find("transform")) tr = v->str_or("");
                    std::string canon = canon_prop_type(pt, tr);
                    if (pn.empty() || canon.empty()) continue;
                    for (auto& cc : pn) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                    e.prop_types.emplace_back(std::move(pn), std::move(canon));
                }
            }
            if (auto const* ev = c.find("events"); ev && ev->is_arr())
            {
                for (auto const& evv : ev->a)
                {
                    std::string en, pe;
                    if (auto const* v = evv.find("name"))            en = v->str_or("");
                    if (auto const* v = evv.find("platform_event"))  pe = v->str_or("");
                    std::string evn = !pe.empty() ? pe : en;
                    // Convention: abstract "onValueChanged" -> "ValueChanged"
                    if (evn.size() > 2 && evn.rfind("on", 0) == 0
                        && std::isupper(static_cast<unsigned char>(evn[2])))
                        evn = evn.substr(2);
                    if (evn.empty()) continue;
                    for (auto& cc : evn) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                    e.event_names.push_back(std::move(evn));
                }
            }
            if (!e.abstract_name.empty()) entries_[e.abstract_name] = std::move(e);
        }
    }

    auto const& entries() const { return entries_; }
    auto const& resource_dictionaries() const { return resource_dictionaries_; }

    catalog_entry const* find(std::string_view name) const
    {
        auto it = entries_.find(std::string(name));
        return it != entries_.end() ? &it->second : nullptr;
    }

    std::string winui3_type(std::string_view name) const
    {
        if (name == "Divider") return "winrt::Microsoft::UI::Xaml::Shapes::Rectangle";
        if (name == "Card") return "winrt::Microsoft::UI::Xaml::Controls::Border";
        if (auto const* e = find(name))
        {
            if (!e->platform.platform_type.empty())
            {
                std::string result = e->platform.platform_type;
                // If catalog type is unqualified (no ::), check fallback for full namespace
                if (result.find("::") == std::string::npos)
                {
                    static const std::unordered_map<std::string, std::string> fallback = {
                        {"Ellipse", "winrt::Microsoft::UI::Xaml::Shapes::Ellipse"},
                        {"Rectangle", "winrt::Microsoft::UI::Xaml::Shapes::Rectangle"},
                        {"Line", "winrt::Microsoft::UI::Xaml::Shapes::Line"},
                        {"Polygon", "winrt::Microsoft::UI::Xaml::Shapes::Polygon"},
                        {"Polyline", "winrt::Microsoft::UI::Xaml::Shapes::Polyline"},
                        {"Path", "winrt::Microsoft::UI::Xaml::Shapes::Path"},
                    };
                    auto it = fallback.find(result);
                    if (it != fallback.end()) return it->second;
                }
                return result;
            }
        }
        // Hardcoded fallback for types not in the catalog
        static const std::unordered_map<std::string, std::string> fallback = {
            {"Ellipse", "winrt::Microsoft::UI::Xaml::Shapes::Ellipse"},
            {"Rectangle", "winrt::Microsoft::UI::Xaml::Shapes::Rectangle"},
            {"Line", "winrt::Microsoft::UI::Xaml::Shapes::Line"},
            {"Polygon", "winrt::Microsoft::UI::Xaml::Shapes::Polygon"},
            {"Polyline", "winrt::Microsoft::UI::Xaml::Shapes::Polyline"},
            {"Path", "winrt::Microsoft::UI::Xaml::Shapes::Path"},
            {"KeyboardAccelerator", "winrt::Microsoft::UI::Xaml::Input::KeyboardAccelerator"},
            {"Uri", "winrt::Windows::Foundation::Uri"},
            {"ImageSource", "winrt::Microsoft::UI::Xaml::Media::ImageSource"},
            {"Row", "winrt::Microsoft::UI::Xaml::Controls::StackPanel"},
            {"Column", "winrt::Microsoft::UI::Xaml::Controls::StackPanel"},
            {"Stack", "winrt::Microsoft::UI::Xaml::Controls::StackPanel"},
            {"Card", "winrt::Microsoft::UI::Xaml::Controls::Border"},
            {"Toggle", "winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch"},
            {"Spacer", "winrt::Microsoft::UI::Xaml::Shapes::Rectangle"},
            {"Divider", "winrt::Microsoft::UI::Xaml::Shapes::Rectangle"},
            {"Input", "winrt::Microsoft::UI::Xaml::Controls::TextBox"},
        };
        auto it = fallback.find(std::string(name));
        if (it != fallback.end()) return it->second;
        return std::string(name);
    }

    std::string winui3_namespace(std::string_view name) const
    {
        if (auto const* e = find(name))
            if (!e->platform.platform_namespace.empty()) return e->platform.platform_namespace;
        return "Microsoft.UI.Xaml.Controls";
    }    bool accepts_children(std::string_view name) const
    {
        if (auto const* e = find(name))
            return e->accepts_children;
        return false;
    }

    std::string composition_kind(std::string_view name) const
    {
        if (auto const* e = find(name))
            return e->composition_kind;
        return "";
    }

    // Returns the single-child property name for a control.
    std::string single_child_prop(std::string_view name) const
    {
        if (name == "ControlExample")
            return "Example";
        if (name == "Border" || name == "Panel" || name == "Card" || name == "ItemContainer")
            return "Child";
        if (name == "StackPanel" || name == "Grid" || name == "Canvas"
            || name == "RelativePanel" || name == "WrapPanel"
            || name == "Column" || name == "Row" || name == "Stack")
            return "Children";
        if (name == "TextBlock" || name == "RichTextBlock")
            return "Inlines";
        if (name == "ItemsRepeater" || name == "ItemsView")
            return "Items";
        if (name == "Viewbox")
            return "Child";
        if (name == "ScrollViewer" || name == "ScrollView" || name == "SwipeControl")
            return "Content";
        if (name == "Tile" || name == "HorizontalScrollContainer")
            return "Source";
        if (name == "ContentControl" || name == "Button" || name == "CheckBox" || name == "RadioButton"
            || name == "ToggleButton" || name == "HyperlinkButton" || name == "AppBarButton"
            || name == "AppBarToggleButton"
            || name == "CommandBarFlyout" || name == "ColorTile"
            || name == "DropDownButton" || name == "SplitButton")
            return "Content";
        if (name == "MenuFlyoutItem" || name == "SplitMenuFlyoutItem"
            || name == "ToggleMenuFlyoutItem" || name == "RadioMenuFlyoutItem"
            || name == "MenuFlyoutSubItem" || name == "SwipeItem"
            || name == "SelectorBarItem" || name == "NavigationViewItem")
            return "Icon";
        if (name == "ItemsControl" || name == "ListView" || name == "GridView" || name == "ComboBox"
            || name == "FlipView" || name == "ListBox" || name == "TreeView" || name == "TabView"
            || name == "MenuBar" || name == "MenuBarItem")
            return "Items";
        // Controls that have no Content property — skip children entirely
        if (name == "ToggleSwitch" || name == "AnimatedIcon"
            || name == "ImplicitAnimationSet" || name == "VariableSizedWrapGrid"
            || name == "ParallaxView" || name == "SemanticZoom"
            || name == "Image" || name == "ImageIcon" || name == "BitmapIcon"
            || name == "FontIcon" || name == "SymbolIcon" || name == "PathIcon"
            || name == "XamlUICommand" || name == "GeometryGroup"
            || name == "SymbolIconSource" || name == "FontIconSource"
            || name == "BitmapIconSource" || name == "PathIconSource"
            || name == "ImageIconSource" || name == "CopyButton")
            return "";
        return "Content";
    }
    // True if the single-child property takes UIElement (not IInspectable)
    // All standalone struct types need from_abi cast since they don't inherit
    bool single_child_needs_cast(std::string_view name) const
    {
        if (name == "ControlExample")
            return false;
        // All controls need cast because no standalone struct inherits UIElement
        return true;
    }
    // Wrap a child variable in a from_abi cast if the property needs UIElement
    std::string cast_child(std::string const& var, std::string const& ctrl_name) const
    {
        if (single_child_needs_cast(ctrl_name))
            return "winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" + var + ".m_unknown.get())";
        return var;
    }

    bool empty() const { return entries_.empty(); }
};


inline std::unordered_map<std::string, std::string> g_type_ns;
inline std::set<std::string> g_project_nss;

// A namespace resolved from loaded metadata is a PLATFORM namespace unless it
// belongs to the project itself (g_project_nss = the project's own component
// namespaces). Platform = WinUI/WinRT framework types OR a third-party control
// library the project mixed in via its own winmd in -input. The old
// Microsoft./Windows. name-prefix test misfiled every third-party control as
// an app type: its setters were skipped, its children wrapped, its <Name>
// typed as <Name>View, and no import was emitted — so the -winui needed-set
// prune dropped the very controls the project uses.
inline bool is_platform_ns(std::string const& ns)
{
    return !ns.empty() && !g_project_nss.count(ns);
}
inline std::set<std::string> g_view_type_names;
inline std::set<std::string> g_missing_types;
inline std::vector<std::string> g_dynamic_resource_dictionaries;
// Metadata-derived control knowledge (keys lowercased; populated by the
// driver from the loaded winmds, consumed by fixup_view_code):
//   type name -> set of lowercased event names (Event table, flattened over
//                base classes + interfaces)
//   type name -> { lowercased property -> metadata type name ("GridLength") }
// Control-specific knowledge stays OUT of the compiler binary: new controls
// and events arrive as metadata, never as new hardcoded lists.
inline std::unordered_map<std::string, std::set<std::string>> g_type_events;
inline std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_type_prop_kinds;
// Enum type knowledge from the loaded winmds: enum type name ->
// (namespace, member names). Populated by the driver next to g_type_ns;
// resolve_typed_value() qualifies enum values from this table, so the
// property -> enum mapping is metadata truth only - never a control list.
inline std::unordered_map<std::string, std::pair<std::string, std::set<std::string>>> g_enum_info;

// Metadata-driven replacement for the old hardcoded numeric-DP name list:
// true iff the loaded winmd says THIS receiver's property setter takes a
// primitive numeric type. Unknown/app-local props are not numeric - the same
// default the old list had for names outside it. Type+metadata only; a new
// control never requires touching this file.
inline bool is_numeric_prop(std::string const& control, std::string const& prop)
{
    std::string lo_c = control;
    for (auto& c : lo_c) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto sc = lo_c.rfind("::");
    if (sc != std::string::npos) lo_c = lo_c.substr(sc + 2);
    std::string lo_p = prop;
    for (auto& c : lo_p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lo_c.empty() || lo_p.empty()) return false;
    auto it = g_type_prop_kinds.find(lo_c);
    if (it == g_type_prop_kinds.end()) return false;
    auto pit = it->second.find(lo_p);
    if (pit == it->second.end()) return false;
    return is_numeric_type_name(pit->second);
}

// Convert template JSON payload to a C++ factory function.

inline bool is_dsl_builtin_control(std::string const& control)
{
    return control == "Row" || control == "Column" || control == "Stack"
        || control == "Card" || control == "Toggle" || control == "Spacer"
        || control == "Divider" || control == "Input" || control == "Checkbox"
        || control == "CheckBox";
}

inline std::string resolve_type_ns(std::string const& control)
{
    if (control == "Row" || control == "Column" || control == "Stack"
        || control == "Card" || control == "Toggle" || control == "Spacer"
        || control == "Input" || control == "Checkbox" || control == "CheckBox")
        return "Microsoft.UI.Xaml.Controls";
    if (control == "Divider")
        return "Microsoft.UI.Xaml.Shapes";
    auto it = g_type_ns.find(control);
    return it != g_type_ns.end() ? it->second : std::string();
}

inline bool is_app_or_unknown_type(std::string const& control)
{
    if (is_dsl_builtin_control(control)) return false;
    std::string ns = resolve_type_ns(control);
    if (ns.empty()) return true;
    if (g_project_nss.count(ns) > 0) return true;
    // Resolved from loaded metadata and not the project's own namespace →
    // platform type: WinUI/WinRT framework OR a third-party control library
    // the project brought in as its own winmd.
    return false;
}

inline std::string try_interpolate_string(
    std::string_view raw_text,
    std::vector<layout_state_var> const& state_vars)
{
    std::string_view text = raw_text;
    if (text.size() >= 3 && text[0] == 'L' && text[1] == '"' && text.back() == '"')
    {
        text = text.substr(2, text.size() - 3);
    }
    else if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    {
        text = text.substr(1, text.size() - 2);
    }

    auto first_brace = text.find('{');
    if (first_brace == std::string::npos || text.find('}', first_brace) == std::string::npos)
        return {};

    // Inline DataTemplate JSON payloads (itemType/body keys) are handled by
    // the template-factory pass - never string-interpolate them: the braces
    // and quotes corrupt both the format string and the emitted literal.
    {
        std::string lo_text(text);
        if (lo_text.find("itemtype") != std::string::npos && lo_text.find("body") != std::string::npos)
            return {};
    }

    if (text.find("{StaticResource") != std::string::npos
        || text.find("{ThemeResource") != std::string::npos
        || text.find("{Binding") != std::string::npos
        || text.find("{x:Bind") != std::string::npos)
        return {};

    std::string fmt_str;
    std::vector<std::string> args;
    size_t i = 0;
    bool found_expr = false;

    while (i < text.size())
    {
        if (text[i] == '{')
        {
            auto close = text.find('}', i + 1);
            if (close != std::string::npos)
            {
                std::string expr(text.substr(i + 1, close - i - 1));
                while (!expr.empty() && expr.front() == ' ') expr.erase(0, 1);
                while (!expr.empty() && expr.back() == ' ') expr.pop_back();

                bool matches = false;
                std::string var_type;
                for (auto const& sv : state_vars)
                {
                    if (expr == sv.name || expr.find(sv.name) != std::string::npos)
                    {
                        matches = true;
                        if (expr == sv.name) var_type = sv.type;
                        else if (sv.type == "double") var_type = "double";
                        break;
                    }
                }

                if (matches)
                {
                    found_expr = true;
                    if (var_type == "double" || expr.find('.') != std::string::npos)
                        fmt_str += "{:.0f}";
                    else
                        fmt_str += "{}";

                    if (var_type == "string" || var_type == "winrt::hstring" || var_type == "std::string" || var_type == "std::wstring")
                        args.push_back("std::wstring(" + expr + ")");
                    else
                        args.push_back("(" + expr + ")");

                    i = close + 1;
                    continue;
                }
            }
        }
        if (text[i] == '\\')
        {
            fmt_str += "\\\\";
            ++i;
            continue;
        }
        if (text[i] == '"')
        {
            fmt_str += "\\\"";
            ++i;
            continue;
        }
        if (text[i] == '{' || text[i] == '}')
            fmt_str += text[i];
        fmt_str += text[i];
        ++i;
    }

    if (!found_expr || args.empty()) return {};

    std::string res = "winrt::hstring(std::format(L\"" + fmt_str + "\"";
    for (auto const& a : args)
        res += ", " + a;
    res += "))";
    return res;
}


// C3: data-driven component-stub catalog.
//
// Stubs the view modules may need: types generated code references but the
// component winmd does not export. Each stub is declared ONCE, as data
// below - gate, namespace, base, ordered body rows - and emitted by one
// loop (emit_component_stubs). The old hand-written if(have(...)) chain is
// gone: adding a stub is a table row, never new emission logic, and gating,
// namespace bracing and ordering all derive from the data.
//
//   gate    qualified name checked against the projected-name set; '@' is
//           the view's app namespace (renaming WinUIGallery -> WinUI_Gallery
//           moves every app stub with it); "" = never gated (the type sits
//           outside every winmd projection and is always stubbed)
//   nspace  namespace the struct is emitted into; "" = global scope
//   base    base type exactly as written after ':'
//   body    ordered rows (sm_* builders); a row is either a structured
//           member or one verbatim line for bespoke shapes
//   blank   extra blank line after the closing `};`
struct stub_member
{
    std::string kind;     // using | ctor | dctor | field | fromabi | method | raw
    std::string ret;      // method: return type
    std::string name;     // method: member name
    std::string params;   // method: parameter list, parens included
    bool        is_const = false;
    std::string dflt;     // method: return expression ("" = derived from ret)
    std::string line;     // raw: verbatim body line (emitted as-is + '\n')
};

struct stub_def
{
    std::string gate;
    std::string nspace;
    std::string type;
    std::string base;
    bool        abs_from_abi = false; // from_abi attaches via ::winrt::com_ptr
    bool        blank_after  = false; // extra blank line after `};`
    std::vector<stub_member> body;
};

inline stub_member sm_using()   { stub_member m; m.kind = "using";   return m; }
inline stub_member sm_ctor()    { stub_member m; m.kind = "ctor";    return m; }
inline stub_member sm_dctor()   { stub_member m; m.kind = "dctor";   return m; }
inline stub_member sm_field()   { stub_member m; m.kind = "field";   return m; }
inline stub_member sm_fromabi() { stub_member m; m.kind = "fromabi"; return m; }
inline stub_member sm_raw(std::string line)
{ stub_member m; m.kind = "raw"; m.line = std::move(line); return m; }
inline stub_member sm_meth(std::string ret, std::string name, std::string params,
                           bool is_const = false, std::string dflt = {})
{
    stub_member m; m.kind = "method";
    m.ret = std::move(ret); m.name = std::move(name);
    m.params = std::move(params); m.is_const = is_const; m.dflt = std::move(dflt);
    return m;
}

inline std::vector<stub_def> const& component_stub_catalog()
{
    static std::vector<stub_def> const cat = [] {
        std::vector<stub_def> v;
        auto add = [&](std::string gate, std::string nspace, std::string type,
                       std::string base, std::vector<stub_member> body,
                       bool abs_from_abi = false, bool blank_after = false) {
            stub_def d;
            d.gate = std::move(gate); d.nspace = std::move(nspace);
            d.type = std::move(type); d.base = std::move(base);
            d.abs_from_abi = abs_from_abi; d.blank_after = blank_after;
            d.body = std::move(body);
            v.push_back(std::move(d));
        };
        std::string const UC  = "winrt::Microsoft::UI::Xaml::Controls::UserControl";
        std::string const CC  = "winrt::Microsoft::UI::Xaml::Controls::ContentControl";
        std::string const INS = "::winrt::IInspectable";
        std::string const CVT = "(::winrt::IInspectable const&, ::winrt::hstring const&, ::winrt::IInspectable const&, ::winrt::hstring const&)";

        // app namespace: gallery controls the winmd never exports
        add("@.ControlExample", "winrt::@", "ControlExample", UC, {
            sm_using(),
            sm_raw("    winrt::Microsoft::UI::Xaml::UIElement Content() const { return static_cast<winrt::Microsoft::UI::Xaml::Controls::UserControl const&>(*this).get_Content(); }"),
            sm_raw("    void Content(winrt::Microsoft::UI::Xaml::UIElement const& value) const { static_cast<winrt::Microsoft::UI::Xaml::Controls::UserControl const&>(*this).put_Content(value); }"),
            sm_raw("    template<class H> winrt::event_token add_Loaded(H const&) const { return {}; }"),
        });
        add("@.ControlExampleSubstitution", "winrt::@", "ControlExampleSubstitution", UC, {
            sm_using(),
            sm_meth("winrt::IInspectable", "Substitution", "()"),
            sm_meth("void", "Substitution", "(winrt::IInspectable const&)"),
        });
        // FeaturedTileLayout: gallery layout control that appears only in the
        // XAML payload of LayoutPanelPage, never as a winmd TypeDef - without
        // this stub the page's bare `FeaturedTileLayout()` fails with C3861.
        add("@.FeaturedTileLayout", "winrt::@", "FeaturedTileLayout",
            "winrt::Microsoft::UI::Xaml::Controls::StackPanel", { sm_using() });

        // CommunityToolkit controls
        add("CommunityToolkit.WinUI.Controls.SettingsCard", "winrt::CommunityToolkit::WinUI::Controls",
            "SettingsCard", CC, {
            sm_using(),
            sm_raw("    struct _sc_state { winrt::Microsoft::UI::Xaml::Controls::TextBlock hdr{winrt::Microsoft::UI::Xaml::Controls::TextBlock::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::TextBlock desc{winrt::Microsoft::UI::Xaml::Controls::TextBlock::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::Border slot{winrt::Microsoft::UI::Xaml::Controls::Border::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::Border icon_slot{winrt::Microsoft::UI::Xaml::Controls::Border::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::Border action_slot{winrt::Microsoft::UI::Xaml::Controls::Border::wrap_tag_t{}}; };"),
            sm_raw("    std::shared_ptr<_sc_state> _st;"),
            sm_raw("    SettingsCard() : ContentControl() {"),
            sm_raw("        _st = std::make_shared<_sc_state>();"),
            sm_raw("        HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);"),
            sm_raw("        HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);"),
            sm_raw("        auto b = winrt::Microsoft::UI::Xaml::Controls::Border();"),
            sm_raw("        b.Background(::theme_brush_th(L\"CardBackgroundFillColorDefaultBrush\"));"),
            sm_raw("        b.BorderBrush(::theme_brush_th(L\"CardStrokeColorDefaultBrush\"));"),
            sm_raw("        b.BorderThickness(winrt::Microsoft::UI::Xaml::Thickness{1,1,1,1});"),
            sm_raw("        b.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{4,4,4,4});"),
            sm_raw("        b.Padding(winrt::Microsoft::UI::Xaml::Thickness{16,14,16,14});"),
            sm_raw("        b.Margin(winrt::Microsoft::UI::Xaml::Thickness{0,0,0,4});"),
            sm_raw("        b.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);"),
            sm_raw("        auto g = winrt::Microsoft::UI::Xaml::Controls::Grid();"),
            sm_raw("        auto c0 = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition(); c0.Width(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto)); g.ColumnDefinitions().Append(c0);"),
            sm_raw("        auto c1 = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition(); c1.Width(winrt::Microsoft::UI::Xaml::GridLength(1, winrt::Microsoft::UI::Xaml::GridUnitType::Star)); g.ColumnDefinitions().Append(c1);"),
            sm_raw("        auto c2 = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition(); c2.Width(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto)); g.ColumnDefinitions().Append(c2);"),
            sm_raw("        auto c3 = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition(); c3.Width(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto)); g.ColumnDefinitions().Append(c3);"),
            sm_raw("        _st->icon_slot = winrt::Microsoft::UI::Xaml::Controls::Border(); _st->icon_slot.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center); _st->icon_slot.Margin(winrt::Microsoft::UI::Xaml::Thickness{0,0,16,0}); _st->icon_slot.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);"),
            sm_raw("        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(_st->icon_slot.m_unknown.get()), 0);"),
            sm_raw("        g.Children().Append(_st->icon_slot);"),
            sm_raw("        auto sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel(); sp.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center); sp.Spacing(2);"),
            sm_raw("        _st->hdr = winrt::Microsoft::UI::Xaml::Controls::TextBlock(); _st->hdr.FontSize(14); _st->hdr.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap); sp.Children().Append(_st->hdr);"),
            sm_raw("        _st->desc = winrt::Microsoft::UI::Xaml::Controls::TextBlock(); _st->desc.FontSize(12); _st->desc.Foreground(::theme_brush_th(L\"TextFillColorSecondaryBrush\")); _st->desc.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap); _st->desc.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed); sp.Children().Append(_st->desc);"),
            sm_raw("        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(sp.m_unknown.get()), 1);"),
            sm_raw("        g.Children().Append(sp);"),
            sm_raw("        _st->slot = winrt::Microsoft::UI::Xaml::Controls::Border(); _st->slot.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center); _st->slot.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right);"),
            sm_raw("        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(_st->slot.m_unknown.get()), 2);"),
            sm_raw("        g.Children().Append(_st->slot);"),
            sm_raw("        _st->action_slot = winrt::Microsoft::UI::Xaml::Controls::Border(); _st->action_slot.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center); _st->action_slot.Margin(winrt::Microsoft::UI::Xaml::Thickness{12,0,0,0}); _st->action_slot.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);"),
            sm_raw("        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(_st->action_slot.m_unknown.get()), 3);"),
            sm_raw("        g.Children().Append(_st->action_slot);"),
            sm_raw("        b.Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(g.m_unknown.get()));"),
            sm_raw("        static_cast<winrt::Microsoft::UI::Xaml::Controls::ContentControl const&>(*this).Content(winrt::IInspectable(b.addref_borrowed()));"),
            sm_raw("    }"),
            sm_raw("    template<class H> winrt::event_token add_Click(H const&) const { return {}; }"),
            sm_raw("    void Header(winrt::IInspectable const& v) const { if (_st && _st->hdr.m_unknown.get()) _st->hdr.Text(winrt::unbox_value_or<winrt::hstring>(v, L\"\")); }"),
            sm_raw("    void Header(winrt::hstring const& v) const { if (_st && _st->hdr.m_unknown.get()) _st->hdr.Text(v); }"),
            sm_raw("    void Description(winrt::IInspectable const& v) const { if (_st && _st->desc.m_unknown.get()) { auto s = winrt::unbox_value_or<winrt::hstring>(v, L\"\"); _st->desc.Text(s); _st->desc.Visibility(s.empty() ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible); } }"),
            sm_raw("    void Description(winrt::hstring const& v) const { if (_st && _st->desc.m_unknown.get()) { _st->desc.Text(v); _st->desc.Visibility(v.empty() ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible); } }"),
            sm_raw("    void Content(winrt::Microsoft::UI::Xaml::UIElement const& v) const { if (_st && _st->slot.m_unknown.get()) _st->slot.Child(v); }"),
            sm_raw("    void Content(winrt::IInspectable const& v) const { if (_st && _st->slot.m_unknown.get() && v.get_abi()) _st->slot.Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(v.get_abi())); }"),
            sm_raw("    void set_icon_helper(winrt::Microsoft::UI::Xaml::Controls::Border const& border, winrt::hstring const& s) const {"),
            sm_raw("        if (!border.m_unknown.get() || s.empty()) return;"),
            sm_raw("        std::wstring ws(s.to_view());"),
            sm_raw("        if (ws.find(L\"BitmapIcon\") != std::wstring::npos || ws.find(L\".png\") != std::wstring::npos) {"),
            sm_raw("            auto bi = winrt::Microsoft::UI::Xaml::Controls::BitmapIcon(); bi.ShowAsMonochrome(false); bi.Width(24); bi.Height(24);"),
            sm_raw("            bi.UriSource(winrt::Windows::Foundation::Uri(L\"ms-appx:///Assets/Tiles/BadgeLogo.png\"));"),
            sm_raw("            border.Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(bi.m_unknown.get())); border.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);"),
            sm_raw("        } else {"),
            sm_raw("            std::wstring glyph = ws; auto p1 = ws.find(L\"Glyph=\\\"\"); if (p1 != std::wstring::npos) { p1 += 7; auto p2 = ws.find(L'\\\"', p1); if (p2 != std::wstring::npos) glyph = ws.substr(p1, p2 - p1); }"),
            sm_raw("            auto fi = winrt::Microsoft::UI::Xaml::Controls::FontIcon(); fi.Glyph(winrt::hstring(glyph)); fi.FontSize(18);"),
            sm_raw("            border.Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(fi.m_unknown.get())); border.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);"),
            sm_raw("        }"),
            sm_raw("    }"),
            sm_raw("    void HeaderIcon(winrt::IInspectable const& v) const { if (_st) set_icon_helper(_st->icon_slot, winrt::unbox_value_or<winrt::hstring>(v, L\"\")); }"),
            sm_raw("    void HeaderIcon(winrt::hstring const& v) const { if (_st) set_icon_helper(_st->icon_slot, v); }"),
            sm_raw("    void ActionIcon(winrt::IInspectable const& v) const { if (_st) set_icon_helper(_st->action_slot, winrt::unbox_value_or<winrt::hstring>(v, L\"\")); }"),
            sm_raw("    void ActionIcon(winrt::hstring const& v) const { if (_st) set_icon_helper(_st->action_slot, v); }"),
            sm_meth("void", "ActionIconToolTip", "(winrt::hstring const&)"),
            sm_meth("void", "IsClickEnabled", "(bool)"),
            sm_meth("void", "ContentAlignment", "(winrt::IInspectable const&)"),
            sm_meth("void", "ContentAlignment", "(winrt::hstring const&)"),
            sm_meth("void", "IsExpanded", "(bool)"),
            sm_meth("bool", "IsExpanded", "()"),
        });
        add("CommunityToolkit.WinUI.Controls.SettingsExpander", "winrt::CommunityToolkit::WinUI::Controls",
            "SettingsExpander", CC, {
            sm_using(),
            sm_raw("    struct _se_state { winrt::Microsoft::UI::Xaml::Controls::TextBlock hdr{winrt::Microsoft::UI::Xaml::Controls::TextBlock::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::TextBlock desc{winrt::Microsoft::UI::Xaml::Controls::TextBlock::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::Border slot{winrt::Microsoft::UI::Xaml::Controls::Border::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::Border icon_slot{winrt::Microsoft::UI::Xaml::Controls::Border::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::FontIcon chevron{winrt::Microsoft::UI::Xaml::Controls::FontIcon::wrap_tag_t{}}; winrt::Microsoft::UI::Xaml::Controls::StackPanel items_panel{winrt::Microsoft::UI::Xaml::Controls::StackPanel::wrap_tag_t{}}; bool is_expanded{false}; bool has_header_content{false}; };"),
            sm_raw("    std::shared_ptr<_se_state> _st;"),
            sm_raw("    SettingsExpander() : ContentControl() {"),
            sm_raw("        _st = std::make_shared<_se_state>();"),
            sm_raw("        HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);"),
            sm_raw("        HorizontalContentAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);"),
            sm_raw("        auto outer = winrt::Microsoft::UI::Xaml::Controls::StackPanel(); outer.Spacing(2); outer.Margin(winrt::Microsoft::UI::Xaml::Thickness{0,0,0,4}); outer.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);"),
            sm_raw("        auto b = winrt::Microsoft::UI::Xaml::Controls::Border();"),
            sm_raw("        b.Background(::theme_brush_th(L\"CardBackgroundFillColorDefaultBrush\"));"),
            sm_raw("        b.BorderBrush(::theme_brush_th(L\"CardStrokeColorDefaultBrush\"));"),
            sm_raw("        b.BorderThickness(winrt::Microsoft::UI::Xaml::Thickness{1,1,1,1});"),
            sm_raw("        b.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{4,4,4,4});"),
            sm_raw("        b.Padding(winrt::Microsoft::UI::Xaml::Thickness{16,14,16,14});"),
            sm_raw("        b.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);"),
            sm_raw("        auto g = winrt::Microsoft::UI::Xaml::Controls::Grid();"),
            sm_raw("        auto c0 = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition(); c0.Width(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto)); g.ColumnDefinitions().Append(c0);"),
            sm_raw("        auto c1 = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition(); c1.Width(winrt::Microsoft::UI::Xaml::GridLength(1, winrt::Microsoft::UI::Xaml::GridUnitType::Star)); g.ColumnDefinitions().Append(c1);"),
            sm_raw("        auto c2 = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition(); c2.Width(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto)); g.ColumnDefinitions().Append(c2);"),
            sm_raw("        auto c3 = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition(); c3.Width(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto)); g.ColumnDefinitions().Append(c3);"),
            sm_raw("        _st->icon_slot = winrt::Microsoft::UI::Xaml::Controls::Border(); _st->icon_slot.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center); _st->icon_slot.Margin(winrt::Microsoft::UI::Xaml::Thickness{0,0,16,0}); _st->icon_slot.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);"),
            sm_raw("        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(_st->icon_slot.m_unknown.get()), 0);"),
            sm_raw("        g.Children().Append(_st->icon_slot);"),
            sm_raw("        auto sp = winrt::Microsoft::UI::Xaml::Controls::StackPanel(); sp.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center); sp.Spacing(2);"),
            sm_raw("        _st->hdr = winrt::Microsoft::UI::Xaml::Controls::TextBlock(); _st->hdr.FontSize(14); _st->hdr.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap); sp.Children().Append(_st->hdr);"),
            sm_raw("        _st->desc = winrt::Microsoft::UI::Xaml::Controls::TextBlock(); _st->desc.FontSize(12); _st->desc.Foreground(::theme_brush_th(L\"TextFillColorSecondaryBrush\")); _st->desc.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap); _st->desc.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed); sp.Children().Append(_st->desc);"),
            sm_raw("        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(sp.m_unknown.get()), 1);"),
            sm_raw("        g.Children().Append(sp);"),
            sm_raw("        _st->slot = winrt::Microsoft::UI::Xaml::Controls::Border(); _st->slot.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center); _st->slot.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Right);"),
            sm_raw("        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(_st->slot.m_unknown.get()), 2);"),
            sm_raw("        g.Children().Append(_st->slot);"),
            sm_raw("        _st->chevron = winrt::Microsoft::UI::Xaml::Controls::FontIcon(); _st->chevron.Glyph(L\"\\uE70D\"); _st->chevron.FontSize(12); _st->chevron.Foreground(::theme_brush_th(L\"TextFillColorSecondaryBrush\")); _st->chevron.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center); _st->chevron.Margin(winrt::Microsoft::UI::Xaml::Thickness{14,0,0,0});"),
            sm_raw("        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(winrt::Microsoft::UI::Xaml::FrameworkElement::wrap_borrowed(_st->chevron.m_unknown.get()), 3);"),
            sm_raw("        g.Children().Append(_st->chevron);"),
            sm_raw("        b.Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(g.m_unknown.get()));"),
            sm_raw("        auto toggle_exp = [st = _st](auto const&, auto const&) {"),
            sm_raw("            if (!st) return;"),
            sm_raw("            st->is_expanded = !st->is_expanded;"),
            sm_raw("            st->items_panel.Visibility(st->is_expanded ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);"),
            sm_raw("            st->chevron.Glyph(st->is_expanded ? L\"\\uE70E\" : L\"\\uE70D\");"),
            sm_raw("        };"),
            sm_raw("        b.add_PointerPressed(toggle_exp);"),
            sm_raw("        b.add_Tapped(toggle_exp);"),
            sm_raw("        outer.Children().Append(b);"),
            sm_raw("        _st->items_panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel(); _st->items_panel.Spacing(2); _st->items_panel.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 2, 0, 0}); _st->items_panel.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch); _st->items_panel.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);"),
            sm_raw("        outer.Children().Append(_st->items_panel);"),
            sm_raw("        static_cast<winrt::Microsoft::UI::Xaml::Controls::ContentControl const&>(*this).Content(winrt::IInspectable(outer.addref_borrowed()));"),
            sm_raw("    }"),
            sm_raw("    void Header(winrt::IInspectable const& v) const { if (_st && _st->hdr.m_unknown.get()) _st->hdr.Text(winrt::unbox_value_or<winrt::hstring>(v, L\"\")); }"),
            sm_raw("    void Header(winrt::hstring const& v) const { if (_st && _st->hdr.m_unknown.get()) _st->hdr.Text(v); }"),
            sm_raw("    void Description(winrt::IInspectable const& v) const { if (_st && _st->desc.m_unknown.get()) { auto s = winrt::unbox_value_or<winrt::hstring>(v, L\"\"); _st->desc.Text(s); _st->desc.Visibility(s.empty() ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible); } }"),
            sm_raw("    void Description(winrt::hstring const& v) const { if (_st && _st->desc.m_unknown.get()) { _st->desc.Text(v); _st->desc.Visibility(v.empty() ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed : winrt::Microsoft::UI::Xaml::Visibility::Visible); } }"),
            sm_raw("    void Content(winrt::Microsoft::UI::Xaml::UIElement const& v) const { if (_st && _st->slot.m_unknown.get()) _st->slot.Child(v); }"),
            sm_raw("    void Content(winrt::IInspectable const& v) const { if (_st && _st->slot.m_unknown.get() && v.get_abi()) _st->slot.Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(v.get_abi())); }"),
            sm_raw("    void Items(winrt::Microsoft::UI::Xaml::UIElement const& v) const { if (_st && _st->items_panel.m_unknown.get()) _st->items_panel.Children().Append(v); }"),
            sm_raw("    void Items(winrt::IInspectable const& v) const { if (_st && _st->items_panel.m_unknown.get() && v.get_abi()) _st->items_panel.Children().Append(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(v.get_abi())); }"),
            sm_raw("    void HeaderIcon(winrt::IInspectable const& v) const {"),
            sm_raw("        if (!_st) return; auto s = winrt::unbox_value_or<winrt::hstring>(v, L\"\"); if (s.empty()) return;"),
            sm_raw("        std::wstring ws(s.to_view());"),
            sm_raw("        if (ws.find(L\"BitmapIcon\") != std::wstring::npos || ws.find(L\".png\") != std::wstring::npos) {"),
            sm_raw("            auto bi = winrt::Microsoft::UI::Xaml::Controls::BitmapIcon(); bi.ShowAsMonochrome(false); bi.Width(24); bi.Height(24);"),
            sm_raw("            bi.UriSource(winrt::Windows::Foundation::Uri(L\"ms-appx:///Assets/Tiles/BadgeLogo.png\"));"),
            sm_raw("            _st->icon_slot.Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(bi.m_unknown.get())); _st->icon_slot.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);"),
            sm_raw("        } else {"),
            sm_raw("            std::wstring glyph = ws; auto p1 = ws.find(L\"Glyph=\\\"\"); if (p1 != std::wstring::npos) { p1 += 7; auto p2 = ws.find(L'\\\"', p1); if (p2 != std::wstring::npos) glyph = ws.substr(p1, p2 - p1); }"),
            sm_raw("            auto fi = winrt::Microsoft::UI::Xaml::Controls::FontIcon(); fi.Glyph(winrt::hstring(glyph)); fi.FontSize(18);"),
            sm_raw("            _st->icon_slot.Child(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(fi.m_unknown.get())); _st->icon_slot.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);"),
            sm_raw("        }"),
            sm_raw("    }"),
            sm_raw("    void HeaderIcon(winrt::hstring const& v) const { HeaderIcon(winrt::box_value(v)); }"),
            sm_raw("    void IsExpanded(bool v) const { if (!_st) return; _st->is_expanded = v; _st->items_panel.Visibility(v ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed); _st->chevron.Glyph(v ? L\"\\uE70E\" : L\"\\uE70D\"); }"),
            sm_raw("    bool IsExpanded() const { return _st ? _st->is_expanded : false; }"),
            sm_meth("void", "ItemsHeader", "(winrt::IInspectable const&)"),
            sm_meth("void", "ItemsFooter", "(winrt::IInspectable const&)"),
        });

        // Microsoft.UI.Xaml.Controls types outside the -winui closure
        add("Microsoft.UI.Xaml.Controls.WrapPanel", "winrt::Microsoft::UI::Xaml::Controls",
            "WrapPanel", "winrt::Microsoft::UI::Xaml::Controls::Panel", {
            sm_using(),
            sm_meth("double", "ItemWidth", "()"),
            sm_meth("void", "ItemWidth", "(double)"),
            sm_meth("double", "ItemHeight", "()"),
            sm_meth("void", "ItemHeight", "(double)"),
        });
        add("Microsoft.UI.Xaml.Controls.ImplicitAnimationSet", "winrt::Microsoft::UI::Xaml::Controls",
            "ImplicitAnimationSet", "winrt::Microsoft::UI::Xaml::UIElement", { sm_using() });
        add("@.CounterControl", "winrt::Microsoft::UI::Xaml::Controls", "CounterControl", UC, {
            sm_using(),
            sm_meth("int32_t", "Count", "()"),
            sm_meth("void", "Count", "(int32_t)"),
        });

        // ungated: no winmd projection ever carries this one
        add("", "winrt::Microsoft::UI::Xaml::Media", "OpacityMaskView", CC, { sm_using() });

        // global-scope component stubs (global scope, ::winrt:: prefix)
        add("@.MenuItemTemplateSelector", "", "MenuItemTemplateSelector", INS, {
            sm_field(), sm_ctor(), sm_fromabi(),
        }, false, true);
        add("CommunityToolkit.WinUI.Controls.SwitchPresenter", "", "SwitchPresenter", CC, {
            sm_dctor(), sm_fromabi(),
            sm_meth("void", "Value", "(winrt::IInspectable const&)", true),
            sm_meth("void", "TargetType", "(winrt::Windows::UI::Xaml::Interop::TypeName const&)", true),
        }, false, true);
        add("@.AnimatedSettingsVisualSource", "", "AnimatedSettingsVisualSource", INS, {
            sm_field(), sm_ctor(),
            sm_meth("void", "Prefix", "(::winrt::hstring const&)"),
            sm_meth("::winrt::hstring", "Prefix", "()"),
            sm_fromabi(),
        }, false, true);
        add("@.LottieLogo1", "", "LottieLogo1", INS, {
            sm_field(), sm_ctor(), sm_fromabi(),
        }, false, true);
        add("@.ValidatedPasswordBox", "", "ValidatedPasswordBox", INS, {
            sm_field(), sm_ctor(), sm_fromabi(),
        }, false, true);
        add("@.CollectionVisibilityConverter", "", "CollectionVisibilityConverter", INS, {
            sm_meth("::winrt::IInspectable", "Convert", CVT),
            sm_meth("::winrt::IInspectable", "ConvertBack", CVT),
        }, false, true);
        add("@.VariedImageSizeLayout", "", "VariedImageSizeLayout",
            "::winrt::Microsoft::UI::Xaml::Controls::Layout", {
            sm_using(),
            sm_meth("double", "MinItemSpacing", "()"),
            sm_meth("void", "MinItemSpacing", "(double)"),
            sm_meth("double", "MinRowSpacing", "()"),
            sm_meth("void", "MinRowSpacing", "(double)"),
        }, false, true);

        std::string const ANIM = "winrt::Microsoft::UI::Xaml::Media::Animation";
        add("@.OffsetAnimation", ANIM, "OffsetAnimation", INS, {
            sm_ctor(),
            sm_meth("double", "Offset", "()"),
            sm_meth("void", "Offset", "(double)"),
            sm_meth("bool", "EnableDependentAnimation", "()"),
            sm_meth("void", "EnableDependentAnimation", "(bool)"),
            sm_meth("void", "EasingMode", "(::winrt::hstring const&)"),
            sm_meth("::winrt::hstring", "EasingMode", "()"),
            sm_meth("void", "From", "(double)"),
            sm_meth("double", "From", "()"),
            sm_meth("void", "To", "(double)"),
            sm_meth("double", "To", "()"),
            sm_meth("void", "Prefix", "(::winrt::hstring const&)"),
            sm_meth("::winrt::hstring", "Prefix", "()"),
        }, false, true);
        add("@.OpacityAnimation", ANIM, "OpacityAnimation", INS, {
            sm_ctor(),
            sm_meth("bool", "EnableDependentAnimation", "()"),
            sm_meth("void", "EnableDependentAnimation", "(bool)"),
            sm_meth("void", "EasingMode", "(::winrt::hstring const&)"),
            sm_meth("::winrt::hstring", "EasingMode", "()"),
            sm_meth("void", "From", "(double)"),
            sm_meth("double", "From", "()"),
            sm_meth("void", "To", "(double)"),
            sm_meth("double", "To", "()"),
            sm_meth("void", "Prefix", "(::winrt::hstring const&)"),
            sm_meth("::winrt::hstring", "Prefix", "()"),
        });

        add("@.EmptyCollectionToObjectConverter", "", "EmptyCollectionToObjectConverter", INS, {
            sm_meth("::winrt::IInspectable", "Convert", CVT),
            sm_meth("::winrt::IInspectable", "ConvertBack", CVT),
        });
        add("@.StringVisibilityConverter", "", "StringVisibilityConverter", INS, {
            sm_meth("::winrt::IInspectable", "Convert", CVT),
            sm_meth("::winrt::IInspectable", "ConvertBack", CVT),
        }, false, true);

        // CollectionViewSource is not part of the -winui closure (UI.Data
        // namespace never reaches the selective projection) but app XAML
        // declares state vars of it (ListViewPage contactCVS) and constructs
        // it as a resource; the view-binding reads on it are commented by R39.
        add("Microsoft.UI.Xaml.Data.CollectionViewSource", "winrt::Microsoft::UI::Xaml::Data",
            "CollectionViewSource", INS, {
            sm_ctor(), sm_field(), sm_fromabi(),
            sm_meth("::winrt::IInspectable", "Source", "()", true),
            sm_meth("void", "Source", "(::winrt::IInspectable const&)"),
            sm_meth("void", "Source", "(::winrt::hstring const&)"),
            sm_meth("bool", "IsSourceGrouped", "()", true),
            sm_meth("void", "IsSourceGrouped", "(bool)"),
            sm_meth("void", "ItemsPath", "(::winrt::hstring const&)"),
            sm_meth("::winrt::IInspectable", "View", "()", true),
            sm_meth("::winrt::IInspectable", "view", "()", true),
        }, true);
        return v;
    }();
    return cat;
}

// Walk the catalog: open/close namespaces as the table moves between them
// (a namespace block is opened even when every stub inside it is gated out,
// exactly like the old chain), gate each row on the projected-name set, and
// emit `struct T : base { ... };` plus its body rows.
inline void emit_component_stubs(std::ostringstream& out, std::string const& ns,
                                 std::set<std::string> const& projected_names)
{
    auto const& cat = component_stub_catalog();
    auto const expand = [&](std::string const& s) {
        std::string r; r.reserve(s.size() + ns.size());
        for (char c : s) { if (c == '@') r += ns; else r += c; }
        return r;
    };
    // Default return expression, matching the old bodies: 0 / false / {}.
    auto const dflt_for = [](std::string const& ret) -> std::string {
        if (ret == "bool") return "false";
        if (ret == "double" || ret == "float" || ret == "int32_t" || ret == "uint32_t") return "0";
        return "{}";
    };
    std::string open;   // currently open namespace ("" = global scope)
    for (auto const& d : cat)
    {
        std::string const ens = expand(d.nspace);
        if (ens != open)
        {
            if (!open.empty()) out << "} // namespace " << open << "\n\n";
            if (!ens.empty())  out << "namespace " << ens << " {\n";
            open = ens;
        }
        std::string const gate = expand(d.gate);
        // Winmd really exports the type: views import it from the app module,
        // so a local stub would be a second definition (C1117).
        if (!gate.empty() && projected_names.count(gate) != 0) continue;
        out << "struct " << d.type << " : " << d.base << " {\n";
        for (auto const& m : d.body)
        {
            if (m.kind == "using")
            {
                auto const pos = d.base.rfind("::");
                std::string const sh = pos == std::string::npos ? d.base : d.base.substr(pos + 2);
                out << "    using " << sh << "::" << sh << ";\n";
            }
            else if (m.kind == "ctor")   out << "    " << d.type << "() {}\n";
            else if (m.kind == "dctor")  out << "    " << d.type << "() : " << d.base << "() {}\n";
            else if (m.kind == "field")  out << "    winrt::com_ptr<void> m_unknown;\n";
            else if (m.kind == "fromabi")
            {
                out << "    static " << d.type << " from_abi(void* ptr) noexcept { "
                    << d.type << " r; r.m_unknown = "
                    << (d.abs_from_abi ? "::winrt::com_ptr<void>" : "winrt::com_ptr<void>")
                    << "::attach(ptr); return r; }\n";
            }
            else if (m.kind == "method")
            {
                out << "    " << m.ret << " " << m.name << m.params << (m.is_const ? " const" : "");
                if (m.ret == "void" && m.dflt.empty()) out << " {}\n";
                else out << " { return " << (m.dflt.empty() ? dflt_for(m.ret) : m.dflt) << "; }\n";
            }
            else out << m.line << "\n";
        }
        out << "};" << (d.blank_after ? "\n\n" : "\n");
    }
    if (!open.empty()) out << "} // namespace " << open << "\n\n";
}


#include "winuic_template.ixx"
#include "winuic_view.ixx"
}
