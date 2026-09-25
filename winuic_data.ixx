// winuic_data.ixx — module winuic_data.
// JSON parser, attribute catalog, and view code generation for consuming
// .winmd files emitted by winuic.  Reads WinUIAttribute and
// WinUIControlCatalogAttribute, parses the layout JSON, and emits C++23
// modules that build the UI tree at runtime.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <sstream>
#include <set>

export module winuic_data;

import winuic_helper;
using namespace winuic;

export namespace winuic
{

// §1  Minimal JSON value (enough to round-trip winuic layout JSON)

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

// §2  JSON parser

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

// §3  Catalog: abstract control → WinUI3 platform type mapping

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
};

class attribute_catalog
{
    std::unordered_map<std::string, catalog_entry> entries_;
public:
    void load_from_json(std::string_view json)
    {
        if (json.empty()) return;
        json_parser p;
        auto root = p.parse(json);
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
            if (auto const* v = c.find("template_resource_key")) e.template_resource_key = v->str_or("");
            if (auto const* v = c.find("resource_dictionary"))   e.resource_dictionary = v->str_or("");
            if (!e.abstract_name.empty()) entries_[e.abstract_name] = std::move(e);
        }
    }

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
        if (name == "ScrollViewer" || name == "SwipeControl")
            return "Content";
        if (name == "ContentControl" || name == "Button" || name == "CheckBox" || name == "RadioButton"
            || name == "ToggleButton" || name == "HyperlinkButton" || name == "AppBarButton"
            || name == "AppBarToggleButton"
            || name == "CommandBarFlyout" || name == "ColorTile" || name == "Tile"
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

// §4  Layout node (parsed from JSON)

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
};

// §5  Parse layout JSON into layout_view

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

// §6  C++23 module code generation

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
                std::vector<std::string> widths;
                std::string tmp = v;
                while (true) {
                    auto wp = tmp.find("Width=\"");
                    if (wp == std::string::npos) break;
                    wp += 7;
                    auto we = tmp.find('"', wp);
                    if (we == std::string::npos) break;
                    widths.push_back(tmp.substr(wp, we - wp));
                    tmp = tmp.substr(we + 1);
                }
                if (!widths.empty()) {
                    std::string prop_name = (klo == "columndefinitions") ? "ColumnDefinitions" : "RowDefinitions";
                    out << indent << "{\n";
                    out << indent << "    auto __gc = " << var_name << ".get_" << prop_name << "();\n";
                    for (auto const& w : widths) {
                        if (w == "Auto") {
                            out << indent << "    { auto __d = winrt::Microsoft::UI::Xaml::ColumnDefinition();\n";
                            out << indent << "      __d.put_Width(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));\n";
                            out << indent << "      __gc.Append(__d); }\n";
                        } else if (!w.empty() && w.back() == '*') {
                            std::string num = w.substr(0, w.size() - 1);
                            if (num.empty()) num = "1";
                            out << indent << "    { auto __d = winrt::Microsoft::UI::Xaml::ColumnDefinition();\n";
                            out << indent << "      __d.put_Width(winrt::Microsoft::UI::Xaml::GridLength(" << num
                                << ", winrt::Microsoft::UI::Xaml::GridUnitType::Star));\n";
                            out << indent << "      __gc.Append(__d); }\n";
                        } else {
                            out << indent << "    { auto __d = winrt::Microsoft::UI::Xaml::ColumnDefinition();\n";
                            out << indent << "      __d.put_Width(winrt::Microsoft::UI::Xaml::GridLength(" << w
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
                        || klo == "overlaybrush" || klo == "selectedhighlightcolor");
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
inline std::unordered_map<std::string, std::string> g_type_ns;
inline std::set<std::string> g_project_nss;
inline std::set<std::string> g_view_type_names;
inline std::set<std::string> g_missing_types;

// Convert template JSON payload to a C++ factory function.
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
    // If namespace is not a known platform namespace, treat as app type.
    // Platform types live under winrt::Microsoft::* or winrt::Windows::*.
    if (!ns.empty() && ns.find("Microsoft") == std::string::npos && ns.find("Windows") == std::string::npos)
        return true;
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

// Generate the .ixx module for a single view.
// Pure modules — zero #include, zero ref new, zero raw pointers.
inline std::string generate_view_module(layout_view const& view, attribute_catalog const& catalog, bool const have_windows_sdk = false,
    std::set<std::string> const& projected_names = {})
{
    std::ostringstream out;
    std::string ns = view.namespace_name.empty() ? "App" : view.namespace_name;
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
    bool is_app_class = (class_name == "App");
    bool is_window_class = class_name.ends_with("Window");
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
    else if (is_window_class)
    {
        // Window classes inherit from Window (not UserControl) for ExtendsContentIntoTitleBar etc.
        base_type = "Microsoft::UI::Xaml::Window";
    }

    // ── Module header ──
    out << "// " << class_name << ".ixx \u2014 module " << ns << "." << class_name << ".\n";
    out << "// generated by cppwinc from winuic-produced .winmd.\n\n";
    out << "module;\n\n";
    // No Win32/WinRT/WDK headers: everything this module needs comes from
    // winrt_base (imported below) or the standard library (`import std;`).
    out << "export module " << ns << "." << class_name << ";\n\n";
    out << "import std;\n";
    out << "export import winrt_base;\n";
    out << "export import winrt.Microsoft.UI.Xaml;\n";
    out << "import winrt.Microsoft.UI.Xaml.Hosting;\n";
    // DispatcherQueueController::CreateOnCurrentThread() - App::Start must
    // create the thread's DispatcherQueue before any XAML object exists.
    out << "import winrt.Microsoft.UI.Dispatching;\n";
    out << "export import winrt.Microsoft.UI.Xaml.Controls;\n";
    out << "import winrt.Microsoft.UI.Xaml.Controls.Primitives;\n";
    out << "export import winrt.Microsoft.UI.Xaml.Media;\n";
    // Windows.UI.Xaml.Interop - TypeName for Frame::Navigate(page registry).
    out << "import winrt.Windows.UI.Xaml.Interop;\n";
    out << "import winrt.Microsoft.UI.Windowing;\n";
    out << "import Windows.Win32.UI.WindowsAndMessaging;\n";
    if (have_windows_sdk)
    {
        // Windows SDK metadata is loaded: the real projections exist, so
        // import them instead of synthesizing Color/FontWeight/FontStyle
        // locally (a local definition would now collide with the import).
        out << "import winrt.Windows.Foundation;\n";
        out << "import winrt.Windows.Foundation.Collections;\n";
        out << "import winrt.Windows.UI;\n";
        out << "import winrt.Windows.UI.Text;\n";
    }
    // Color is not projected (no SDK metadata) - define locally
    if (!have_windows_sdk)
    {
        out << "namespace winrt::Windows::UI {\n";
        out << "struct Color { std::uint8_t A{255}, R{0}, G{0}, B{0}; };\n";
        out << "} // namespace winrt::Windows::UI\n\n";
    }
    // Navigation windows import the page modules they navigate to (driver-
    // computed from the project's own views - no hardcoded page names).
    if (is_window_class && !view.nav_pages.empty())
    {
        std::set<std::string> seen_page_mods;
        for (auto const& [tag, page] : view.nav_pages)
        {
            // The page view's module is exactly "<Ns>.<Page>" (same dotted
            // name as the page entry).
            if (page.empty() || !seen_page_mods.insert(page).second) continue;
            out << "import " << page << ";\n";
        }
    }
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
    out << "        void (*notify_theme_change)() = nullptr;\n";
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
    out << "    static const winrt::winrt_guid iid_map{0x3C26D538,0x02B8,0x5C3B,{0x9A,0x25,0x8E,0x49,0x08,0x15,0xCA,0x3D}};\n";
    out << "    void* map_raw = nullptr;\n";
    out << "    if (winrt::impl::query_interface(inspectable_raw, iid_map, &map_raw) < 0 || !map_raw) return winrt::IInspectable{};\n";
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
    out << "    try {\n";
    out << "        auto app = winrt::Microsoft::UI::Xaml::Application::get_Current();\n";
    out << "        if (app.get_abi() == nullptr) return fallback;\n";
    out << "        auto res = app.get_Resources();\n";
    out << "        if (res.get_abi() == nullptr) return fallback;\n";
    out << "        bool is_light = is_current_light_theme();\n";
    out << "        wchar_t const* th1 = is_light ? L\"Light\" : L\"Dark\";\n";
    out << "        wchar_t const* th2 = L\"Default\";\n";
    out << "        wchar_t const* th3 = is_light ? L\"Dark\" : L\"Light\";\n";
    out << "        winrt::IInspectable v = theme_search_rd_recursive(res, key, th1, th2, th3, 0);\n";
    out << "        if (v) return v;\n";
    out << "    } catch (...) {}\n";
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
    out << "    if (k.find(L\"CardBackground\") != std::wstring_view::npos || k.find(L\"LayerFillColorDefault\") != std::wstring_view::npos)\n";
    out << "        return get_or_create(g_theme_state.card_bg, winrt::Windows::UI::Color{255, 255, 255, 255}, winrt::Windows::UI::Color{255, 43, 43, 43});\n";
    out << "    if (k.find(L\"CardStroke\") != std::wstring_view::npos || k.find(L\"SurfaceStroke\") != std::wstring_view::npos)\n";
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
    out << "    if (!v) return winrt::Microsoft::UI::Xaml::Style::from_abi(nullptr);\n";
    out << "    winrt::IUnknown::addref_raw(v.get_abi());\n";
    out << "    return winrt::Microsoft::UI::Xaml::Style::from_abi(v.get_abi());\n";
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
    out << "    if (!v) return winrt::Microsoft::UI::Xaml::DataTemplate::from_abi(nullptr);\n";
    out << "    winrt::IUnknown::addref_raw(v.get_abi());\n";
    out << "    return winrt::Microsoft::UI::Xaml::DataTemplate::from_abi(v.get_abi());\n";
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
    // Custom namespace stubs (WinUIGallery, CommunityToolkit) are kept below.
    // Missing component type stubs — types referenced in codegen but not in component module.
    // Each is gated on the projected-name set: when the app winmd actually exports the
    // type, views import it from the WinUIGallery module — a local stub would be a
    // second definition of the same name (C1117 'symbol already defined' on import).
    auto const have = [&projected_names](char const* n) { return projected_names.count(n) == 0; };
    out << "namespace winrt::WinUIGallery {\n";
    if (have("WinUIGallery.ControlExample")) {
    out << "struct ControlExample : winrt::Microsoft::UI::Xaml::Controls::UserControl {\n";
    out << "    using UserControl::UserControl;\n";
    out << "    winrt::Microsoft::UI::Xaml::UIElement Content() const { return static_cast<winrt::Microsoft::UI::Xaml::Controls::UserControl const&>(*this).get_Content(); }\n";
    out << "    void Content(winrt::Microsoft::UI::Xaml::UIElement const& value) const { static_cast<winrt::Microsoft::UI::Xaml::Controls::UserControl const&>(*this).put_Content(value); }\n";
    out << "    template<class H> winrt::event_token add_Loaded(H const&) const { return {}; }\n";
    out << "};\n";
    }
    if (have("WinUIGallery.ControlExampleSubstitution")) {
    out << "struct ControlExampleSubstitution : winrt::Microsoft::UI::Xaml::Controls::UserControl {\n";
    out << "    using UserControl::UserControl;\n";
    out << "    winrt::IInspectable Substitution() { return {}; }\n";
    out << "    void Substitution(winrt::IInspectable const&) {}\n";
    out << "};\n";
    }
    out << "} // namespace winrt::WinUIGallery\n\n";
    // SettingsCard/SettingsExpander stubs (CommunityToolkit) - gated on the
    // projected-name set so a winmd that really exports them is not shadowed.
    out << "namespace winrt::CommunityToolkit::WinUI::Controls {\n";
    if (have("CommunityToolkit.WinUI.Controls.SettingsCard")) {
    out << "struct SettingsCard : winrt::Microsoft::UI::Xaml::Controls::ContentControl {\n";
    out << "    using ContentControl::ContentControl;\n";
    out << "    template<class H> winrt::event_token add_Click(H const&) const { return {}; }\n";
    out << "    void Header(winrt::IInspectable const&) {}\n";
    out << "    void Description(winrt::IInspectable const&) {}\n";
    out << "    void IsExpanded(bool) {}\n";
    out << "    bool IsExpanded() { return false; }\n";
    out << "};\n";
    }
    if (have("CommunityToolkit.WinUI.Controls.SettingsExpander")) {
    out << "struct SettingsExpander : winrt::Microsoft::UI::Xaml::Controls::ContentControl {\n";
    out << "    using ContentControl::ContentControl;\n";
    out << "    void Header(winrt::IInspectable const&) {}\n";
    out << "    void Description(winrt::IInspectable const&) {}\n";
    out << "    bool IsExpanded() { return false; }\n";
    out << "    void IsExpanded(bool) {}\n";
    out << "    void Items(winrt::IInspectable const&) {}\n";
    out << "};\n";
    }
    out << "} // namespace winrt::CommunityToolkit::WinUI::Controls\n\n";
    // Additional missing type stubs - each gated on the projected-name set
    out << "namespace winrt::Microsoft::UI::Xaml::Controls {\n";
    if (have("Microsoft.UI.Xaml.Controls.WrapPanel")) {
    out << "struct WrapPanel : winrt::Microsoft::UI::Xaml::Controls::Panel {\n";
    out << "    using Panel::Panel;\n";
    out << "    double ItemWidth() { return 0; }\n";
    out << "    void ItemWidth(double) {}\n";
    out << "    double ItemHeight() { return 0; }\n";
    out << "    void ItemHeight(double) {}\n";
    out << "};\n";
    }
    if (have("Microsoft.UI.Xaml.Controls.ImplicitAnimationSet")) {
    out << "struct ImplicitAnimationSet : winrt::Microsoft::UI::Xaml::UIElement {\n";
    out << "    using UIElement::UIElement;\n";
    out << "};\n";
    }
    if (have("WinUIGallery.CounterControl")) {
    out << "struct CounterControl : winrt::Microsoft::UI::Xaml::Controls::UserControl {\n";
    out << "    using UserControl::UserControl;\n";
    out << "    int32_t Count() { return 0; }\n";
    out << "    void Count(int32_t) {}\n";
    out << "};\n";
    }
    out << "} // namespace winrt::Microsoft::UI::Xaml::Controls\n\n";
    // OpacityMaskView stub
    out << "namespace winrt::Microsoft::UI::Xaml::Media {\n";
    out << "struct OpacityMaskView : winrt::Microsoft::UI::Xaml::Controls::ContentControl {\n";
    out << "    using ContentControl::ContentControl;\n";
    out << "};\n";
    out << "} // namespace winrt::Microsoft::UI::Xaml::Media\n\n";
    // All stubs in global scope with ::winrt:: prefix (namespace already closed above).
    // Stubs carry m_unknown/from_abi so they can be passed to UIElement::from_abi and
    // similar (projected setters take base types, and standalone structs don't inherit).
    // CopyButton is a real component class in the merged module (CopyButton : Button) -
    // NEVER fabricate a header stub for it: pages import WinUIGallery and the stub would
    // shadow/ambiguate the real type.
    if (have("WinUIGallery.MenuItemTemplateSelector")) {
    out << "struct MenuItemTemplateSelector : ::winrt::IInspectable {\n";
    out << "    winrt::com_ptr<void> m_unknown;\n";
    out << "    MenuItemTemplateSelector() {}\n";
    out << "    static MenuItemTemplateSelector from_abi(void* ptr) noexcept { MenuItemTemplateSelector r; r.m_unknown = winrt::com_ptr<void>::attach(ptr); return r; }\n";
    out << "};\n\n";
    }
    if (have("CommunityToolkit.WinUI.Controls.SwitchPresenter")) {
    out << "struct SwitchPresenter : winrt::Microsoft::UI::Xaml::Controls::ContentControl {\n";
    out << "    SwitchPresenter() : winrt::Microsoft::UI::Xaml::Controls::ContentControl() {}\n";
    out << "    static SwitchPresenter from_abi(void* ptr) noexcept { SwitchPresenter r; r.m_unknown = winrt::com_ptr<void>::attach(ptr); return r; }\n";
    out << "    void Value(winrt::IInspectable const&) const {}\n";
    out << "    void TargetType(winrt::Windows::UI::Xaml::Interop::TypeName const&) const {}\n";
    out << "};\n\n";
    }
    if (have("WinUIGallery.AnimatedSettingsVisualSource")) {
    out << "struct AnimatedSettingsVisualSource : ::winrt::IInspectable {\n";
    out << "    winrt::com_ptr<void> m_unknown;\n";
    out << "    AnimatedSettingsVisualSource() {}\n";
    out << "    void Prefix(::winrt::hstring const&) {}\n";
    out << "    ::winrt::hstring Prefix() { return {}; }\n";
    out << "    static AnimatedSettingsVisualSource from_abi(void* ptr) noexcept { AnimatedSettingsVisualSource r; r.m_unknown = winrt::com_ptr<void>::attach(ptr); return r; }\n";
    out << "};\n\n";
    }
    if (have("WinUIGallery.LottieLogo1")) {
    out << "struct LottieLogo1 : ::winrt::IInspectable {\n";
    out << "    winrt::com_ptr<void> m_unknown;\n";
    out << "    LottieLogo1() {}\n";
    out << "    static LottieLogo1 from_abi(void* ptr) noexcept { LottieLogo1 r; r.m_unknown = winrt::com_ptr<void>::attach(ptr); return r; }\n";
    out << "};\n\n";
    }
    if (have("WinUIGallery.ValidatedPasswordBox")) {
    out << "struct ValidatedPasswordBox : ::winrt::IInspectable {\n";
    out << "    winrt::com_ptr<void> m_unknown;\n";
    out << "    ValidatedPasswordBox() {}\n";
    out << "    static ValidatedPasswordBox from_abi(void* ptr) noexcept { ValidatedPasswordBox r; r.m_unknown = winrt::com_ptr<void>::attach(ptr); return r; }\n";
    out << "};\n\n";
    }
    if (have("WinUIGallery.CollectionVisibilityConverter")) {
    out << "struct CollectionVisibilityConverter : ::winrt::IInspectable {\n";
    out << "    ::winrt::IInspectable Convert(::winrt::IInspectable const&, ::winrt::hstring const&, ::winrt::IInspectable const&, ::winrt::hstring const&) { return {}; }\n";
    out << "    ::winrt::IInspectable ConvertBack(::winrt::IInspectable const&, ::winrt::hstring const&, ::winrt::IInspectable const&, ::winrt::hstring const&) { return {}; }\n";
    out << "};\n\n";
    }
    if (have("WinUIGallery.VariedImageSizeLayout")) {
    out << "struct VariedImageSizeLayout : ::winrt::Microsoft::UI::Xaml::Controls::Layout {\n";
    out << "    using Layout::Layout;\n";
    out << "    double MinItemSpacing() { return 0; }\n";
    out << "    void MinItemSpacing(double) {}\n";
    out << "    double MinRowSpacing() { return 0; }\n";
    out << "    void MinRowSpacing(double) {}\n";
    out << "};\n\n";
    }
    out << "namespace winrt::Microsoft::UI::Xaml::Media::Animation {\n";
    if (have("WinUIGallery.OffsetAnimation")) {
    out << "struct OffsetAnimation : ::winrt::IInspectable {\n";
    out << "    OffsetAnimation() {}\n";
    out << "    double Offset() { return 0; }\n";
    out << "    void Offset(double) {}\n";
    out << "    bool EnableDependentAnimation() { return false; }\n";
    out << "    void EnableDependentAnimation(bool) {}\n";
    out << "    void EasingMode(::winrt::hstring const&) {}\n";
    out << "    ::winrt::hstring EasingMode() { return {}; }\n";
    out << "    void From(double) {}\n";
    out << "    double From() { return 0; }\n";
    out << "    void To(double) {}\n";
    out << "    double To() { return 0; }\n";
    out << "    void Prefix(::winrt::hstring const&) {}\n";
    out << "    ::winrt::hstring Prefix() { return {}; }\n";
    out << "};\n\n";
    }
    if (have("WinUIGallery.OpacityAnimation")) {
    out << "struct OpacityAnimation : ::winrt::IInspectable {\n";
    out << "    OpacityAnimation() {}\n";
    out << "    bool EnableDependentAnimation() { return false; }\n";
    out << "    void EnableDependentAnimation(bool) {}\n";
    out << "    void EasingMode(::winrt::hstring const&) {}\n";
    out << "    ::winrt::hstring EasingMode() { return {}; }\n";
    out << "    void From(double) {}\n";
    out << "    double From() { return 0; }\n";
    out << "    void To(double) {}\n";
    out << "    double To() { return 0; }\n";
    out << "    void Prefix(::winrt::hstring const&) {}\n";
    out << "    ::winrt::hstring Prefix() { return {}; }\n";
    out << "};\n";
    }
    out << "} // namespace winrt::Microsoft::UI::Xaml::Media::Animation\n\n";
    // EmptyCollectionToObjectConverter, StringVisibilityConverter stubs (global scope)
    if (have("WinUIGallery.EmptyCollectionToObjectConverter")) {
    out << "struct EmptyCollectionToObjectConverter : ::winrt::IInspectable {\n";
    out << "    ::winrt::IInspectable Convert(::winrt::IInspectable const&, ::winrt::hstring const&, ::winrt::IInspectable const&, ::winrt::hstring const&) { return {}; }\n";
    out << "    ::winrt::IInspectable ConvertBack(::winrt::IInspectable const&, ::winrt::hstring const&, ::winrt::IInspectable const&, ::winrt::hstring const&) { return {}; }\n";
    out << "};\n";
    }
    if (have("WinUIGallery.StringVisibilityConverter")) {
    out << "struct StringVisibilityConverter : ::winrt::IInspectable {\n";
    out << "    ::winrt::IInspectable Convert(::winrt::IInspectable const&, ::winrt::hstring const&, ::winrt::IInspectable const&, ::winrt::hstring const&) { return {}; }\n";
    out << "    ::winrt::IInspectable ConvertBack(::winrt::IInspectable const&, ::winrt::hstring const&, ::winrt::IInspectable const&, ::winrt::hstring const&) { return {}; }\n";
    out << "};\n\n";
    }
    // CollectionViewSource is not part of the -winui closure (UI.Data namespace
    // never reaches the selective projection) but app XAML declares state vars
    // of it (ListViewPage contactsCVS) and constructs it as a resource. Emit
    // an IInspectable stub (global scope, like the other component stubs) so
    // the member declaration and `auto X = CollectionViewSource();` resolve;
    // the view-binding reads on it are commented by R39.
    out << "namespace winrt::Microsoft::UI::Xaml::Data {\n";
    if (have("Microsoft.UI.Xaml.Data.CollectionViewSource")) {
    out << "struct CollectionViewSource : ::winrt::IInspectable {\n";
    out << "    CollectionViewSource() {}\n";
    out << "    ::winrt::com_ptr<void> m_unknown;\n";
    out << "    static CollectionViewSource from_abi(void* ptr) noexcept { CollectionViewSource r; r.m_unknown = ::winrt::com_ptr<void>::attach(ptr); return r; }\n";
    out << "    ::winrt::IInspectable Source() const { return {}; }\n";
    out << "    void Source(::winrt::IInspectable const&) {}\n";
    out << "    void Source(::winrt::hstring const&) {}\n";
    out << "    bool IsSourceGrouped() const { return false; }\n";
    out << "    void IsSourceGrouped(bool) {}\n";
    out << "    void ItemsPath(::winrt::hstring const&) {}\n";
    out << "    ::winrt::IInspectable View() const { return {}; }\n";
    out << "    ::winrt::IInspectable view() const { return {}; }\n";
    out << "};\n";
    }
    out << "} // namespace winrt::Microsoft::UI::Xaml::Data\n\n";

    // Helper: append to any IVector-based collection. Prefer the typed
    // .Append() member when the wrapper exposes one (interface-typed returns
    // AND runtime-class wrappers whose default interface is IVector<T> — the
    // typed call QIs to the right interface). The raw vtbl[13] path is only
    // a fallback: it assumes the ABI pointer is IVector-shaped, which is
    // false for class-typed getters whose wrapper holds the plain
    // IInspectable pointer (e.g. ComboBox.Items() -> ItemCollection) —
    // calling slot 13 on that reads past the vtable (garbage target, AV).
    out << "// Projected IVector<T>/IObservableVector<T> wrappers expose get_abi() but don't\n";
    out << "// convert to IInspectable; collection classes (InlineCollection, UIElementCollection)\n";
    out << "// convert to IInspectable but have no get_abi(). Route both to their ABI pointer.\n";
    out << "template <typename TColl>\n";
    out << "    requires requires(TColl const& c) { c.get_abi(); }\n";
    out << "static void* collection_abi(TColl const& c) { return c.get_abi(); }\n";
    out << "static void* collection_abi(winrt::IInspectable const& c) { return c.get_abi(); }\n";
    out << "template <typename TColl, typename TItem>\n";
    out << "    requires requires(TColl& c, TItem const& i) { c.Append(i); }\n";
    out << "static void inspectable_append(TColl collection, TItem const& item) {\n";
    out << "    collection.Append(item);\n";
    out << "}\n";
    out << "template <typename TColl>\n";
    out << "    requires (!requires(TColl& c, winrt::IInspectable const& i) { c.Append(i); })\n";
    out << "static void inspectable_append(TColl const& collection, winrt::IInspectable const& item) {\n";
    out << "    void* col_ptr = collection_abi(collection);\n";
    out << "    auto** vtbl = *reinterpret_cast<void***>(col_ptr);\n";
    out << "    // IVector vtable: IInspectable(6 slots) + IVector(Append at slot 13)\n";
    out << "    reinterpret_cast<void(__stdcall*)(void*, void*)>(vtbl[13])(col_ptr, item.get_abi());\n";
    out << "}\n\n";
    // Helper: convert bool to Visibility enum
    out << "static winrt::Microsoft::UI::Xaml::Visibility make_visibility(bool v) {\n";
    out << "    return v ? winrt::Microsoft::UI::Xaml::Visibility::Visible : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;\n";
    out << "}\n\n";
    // Helper: convert string name to Symbol enum value
    out << "static winrt::Microsoft::UI::Xaml::Controls::Symbol symbol_from_name(winrt::hstring const& name) {\n";
    out << "    auto sv = name.to_view();\n";
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

    if (is_app_class || is_shell) {
        out << "import winrt.Microsoft.UI.Xaml.Markup;\n";
        // App module's on_launched calls launch_<MainWindow>() which lives in
        // the separate main-window view module - import it (shell modules are
        // their own App module and already import every view below).
        if (is_app_class && !is_shell && view.has_main_window && !view.main_window_class.empty())
            out << "import " << ns << "." << view.main_window_class << ";\n";
    }
    // App-shell module: the window class in this same module constructs the
    // project's page views directly (bare-name accessors from the page
    // modules), so every other view module is imported here.
    if (is_shell)
    {
        for (auto const& vn : g_view_type_names)
            if (vn != class_name) out << "import " << ns << "." << vn << ";\n";
    }
    // All view modules import the component module for custom control types
    out << "import " << ns << ";\n";
    // Sibling view modules this view instantiates (get_<Name>() accessors).
    // Without these the emitted get_<X>() calls have no declaration - C3861.
    for (auto const& comp_imp : view.component_view_imports)
        out << "import " << comp_imp << ";\n";
    out << "using namespace winrt::" << ns << ";\n";
    out << "\n";
    // Note: view class is not exported to avoid conflict with component module struct.
    // The component module exports the WinRT activation wrapper used by make<T>() in App.
    // Templates use factory functions: no XamlReader for ItemTemplate.
    bool needs_xaml_reader = false;
    std::function<void(layout_node const&)> check_tpl = [&](layout_node const& nd) {
        for (auto const& [rk, rv] : nd.raw_attrs)
            if (rv.find("GroupStyle") != std::string::npos
                || (rv.find("DataTemplate") != std::string::npos && rv.find("load_xaml") != std::string::npos))
                { needs_xaml_reader = true; return; }
        for (auto const& ch : nd.children) check_tpl(ch);
    };
    for (auto const& rn : view.root) check_tpl(rn);
    if (needs_xaml_reader)
        out << "import winrt.Microsoft.UI.Xaml.Markup;\n";
    out << "using namespace winrt;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Controls;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Documents;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Media;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Media::Animation;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Shapes;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Input;\n";
    out << "using namespace winrt::Microsoft::UI::Xaml::Data;\n";
    out << "using namespace winrt::Windows::Foundation;\n";
    out << "using namespace winrt::Windows::UI;\n";
    out << "using namespace winrt::CommunityToolkit::WinUI::Controls;\n";


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
        out << "    static void boot_log(char const*) {}\n";
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
            // Settings pages wire ThemeComboBox + CompactToggle right after
            // the tree is built (same ctor position as the original
            // code-behind's InitializeComponent + wire-up ordering).
            if (element_names.count("ThemeComboBox"))
                out << "        _wire_settings_behavior();\n";
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
        // Initial navigation: select the first menu item; the resulting
        // SelectionChanged raises navigate the first page (the reference
        // apps' OnNavViewLoaded pattern). Emitted here only when the view
        // wires no Loaded callback - otherwise it is appended to that
        // callback's body below (the Frame must be Loaded to Navigate).
        bool has_loaded_cb = false;
        for (auto const& [cbn, cbs] : view.callbacks)
            if (cbn.find("Loaded") != std::string::npos) { has_loaded_cb = true; break; }
        if (!view.nav_pages.empty() && (element_names.count("NavView") || element_names.count("NavigationViewControl")) && !has_loaded_cb)
        {
            std::string nv = element_names.count("NavigationViewControl") ? "NavigationViewControl" : "NavView";
            out << "        {\n";
            out << "            auto items = " << nv << ".MenuItems();\n";
            out << "            if (items && items.get_Size() > 0)\n";
            out << "            {\n";
            out << "                auto first = items.GetAt(0);\n";
            out << "                if (first) " << nv << ".SelectedItem(first);\n";
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
        std::unordered_map<std::string, std::string> element_named_types;
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
                    bool platform_type = ns.rfind("Microsoft.", 0) == 0 || ns.rfind("Windows.", 0) == 0;
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
                                if (mnit != g_type_ns.end()
                                    && (mnit->second.rfind("Microsoft.", 0) == 0 || mnit->second.rfind("Windows.", 0) == 0))
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
                auto en = element_named_types.find(sv.name);
                if (en != element_named_types.end())
                    cxx_type = en->second;
                else if (sv.type == "string" || cxx_type == "winrt::hstring")
                {
                    auto it = inferred_types.find(sv.name);
                    if (it != inferred_types.end()) cxx_type = it->second;
                }
                out << "    " << cxx_type << " " << sv.name;
                if (!sv.initial.empty()) {
                    // If type is winrt::hstring, ensure string literal uses L prefix
                    if (cxx_type == "winrt::hstring" && sv.initial.front() == '"' && sv.initial.find("L\"") == std::string::npos)
                        out << " = L" << sv.initial;
                    else
                        out << " = " << sv.initial;
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
                bool platform_type = ns.rfind("Microsoft.", 0) == 0 || ns.rfind("Windows.", 0) == 0;
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
                            if (mnit != g_type_ns.end()
                                && (mnit->second.rfind("Microsoft.", 0) == 0 || mnit->second.rfind("Windows.", 0) == 0))
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
    out << "    void update_bindings()\n    {\n";
    out << "        for (auto const& fn : m_bindings) fn();\n";
    if (is_shell)
        out << "        update_router();\n";
    out << "    }\n\n";

    if (is_shell)
    {
        out << "    winrt::Microsoft::UI::Xaml::Controls::Grid m_router_panel;\n";
        out << "    void update_router()\n    {\n";
        out << "        if (!m_router_panel.get_abi()) return;\n";
        out << "        m_router_panel.Children().Clear();\n";
        out << "        auto theme = is_current_light_theme() ? winrt::Microsoft::UI::Xaml::ElementTheme::Light : winrt::Microsoft::UI::Xaml::ElementTheme::Dark;\n";
        out << "        if (selectedNav == 0)\n";
        out << "        {\n";
        out << "            auto& page = get_DashboardPage();\n";
        out << "            static bool s_dp_inited = false;\n";
        out << "            if (!s_dp_inited)\n";
        out << "            {\n";
        out << "                s_dp_inited = true;\n";
        out << "                page.count = count;\n";
        out << "                page.sliderValue = sliderValue;\n";
        out << "                page.isToggleOn = isToggleOn;\n";
        out << "                page.progressValue = progressValue;\n";
        out << "                page.update_bindings();\n";
        out << "            }\n";
        out << "            page.RequestedTheme(theme);\n";
        out << "            page.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch);\n";
        out << "            page.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);\n";
        out << "            m_router_panel.Children().Append(page);\n";
        out << "        }\n";
        out << "        else if (selectedNav == 1) {\n";
        out << "            auto& page = get_TodoListPage();\n";
        out << "            page.RequestedTheme(theme);\n";
        out << "            page.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch);\n";
        out << "            page.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);\n";
        out << "            m_router_panel.Children().Append(page);\n";
        out << "        }\n";
        out << "        else if (selectedNav == 2)\n";
        out << "        {\n";
        out << "            auto& page = get_SettingsPage();\n";
        out << "            static bool s_sp_inited = false;\n";
        out << "            if (!s_sp_inited)\n";
        out << "            {\n";
        out << "                s_sp_inited = true;\n";
        out << "                page.isDarkMode = is_current_dark_theme();\n";
        out << "                page.messageText = messageText;\n";
        out << "                page.update_bindings();\n";
        out << "            }\n";
        out << "            else\n";
        out << "            {\n";
        out << "                page.isDarkMode = is_current_dark_theme();\n";
        out << "                page.update_bindings();\n";
        out << "            }\n";
        out << "            page.RequestedTheme(theme);\n";
        out << "            page.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch);\n";
        out << "            page.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);\n";
        out << "            m_router_panel.Children().Append(page);\n";
        out << "        }\n";
        out << "        else {\n";
        out << "            auto& page = get_AboutPage();\n";
        out << "            page.RequestedTheme(theme);\n";
        out << "            page.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Stretch);\n";
        out << "            page.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);\n";
        out << "            m_router_panel.Children().Append(page);\n";
        out << "        }\n";
        out << "    }\n\n";
    }

    out << "    // Build the UI tree from the winuic layout.\n";
    int tpl_counter = 0;
    std::vector<template_factory_result> pending_factory_funcs;
    out << "    void BuildUI()\n    {\n";
    out << "        boot_log(\"ui:begin\");\n";
    out << "#define WINUIC_UI_STEP(s) do { winrt::boot_log(s); } while (0)\n";

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
        out << "        } catch (...) {}\n";
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

    out << "\n        // build the UI tree\n";

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
                        || lo_t.find("number") != std::string::npos);
                    if (!numeric) break;
                    std::string rest = prop;
                    while (!rest.empty() && rest.back() == ')') rest.pop_back();
                    while (!rest.empty() && rest.back() == '(') rest.pop_back();
                    if (rest == "Value" || rest == "value" || rest.empty())
                        return sv.name;
                    break;
                }
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
        if (g_view_type_names.count(n.control) && n.control != class_name)
        {
            // Sibling app view: construct a FRESH instance through the
            // component module's WinRT wrapper (activation + factory bridge),
            // not get_<Name>() - that returns the app-lifetime singleton and
            // appending it twice would reparent it out of the first host.
            // Qualify with the view namespace: platform types (e.g.
            // Media::OpacityMaskView) leak into the unqualified scope via
            // export-imports and make the bare name ambiguous (C2872).
            std::string view_ns_prefix;
            if (!view.namespace_name.empty())
            {
                std::string qvns;
                for (char qc : view.namespace_name)
                    qvns += (qc == '.') ? "::" : std::string(1, qc);
                view_ns_prefix = "winrt::" + qvns + "::";
            }
            out << indent << "auto " << var_name << " = " << view_ns_prefix << n.control << "();\n";
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
        else if (n.control == "ScrollViewer")
        {
            out << indent << var_name << ".VerticalScrollBarVisibility(winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Visible);\n";
            out << indent << var_name << ".VerticalScrollMode(winrt::Microsoft::UI::Xaml::Controls::ScrollMode::Enabled);\n";
            out << indent << var_name << ".HorizontalScrollBarVisibility(winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled);\n";
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
            }
        }

        // Emit Grid.RowDefinitions from raw XML fragment if present
        if (n.control == "Grid")
        {
            bool has_row_defs = false;
            for (auto const& [rk, rv] : n.raw_attrs)
            {
                if (rk == "RowDefinitions") has_row_defs = true;
                if (rk == "RowDefinitions" || rk == "ColumnDefinitions")
                {
                    std::string defs = rv;
                    if (defs.size() >= 2 && defs.front() == '"' && defs.back() == '"')
                        defs = defs.substr(1, defs.size() - 2);
                    // Unescape backslash-escaped quotes
                    for (size_t i = 0; i < defs.size(); ++i)
                        if (defs[i] == '\\' && i + 1 < defs.size() && defs[i+1] == '"')
                            { defs[i+1] = '"'; defs.erase(i, 1); --i; }
                    bool is_row = (rk == "RowDefinitions");
                    std::string dim = is_row ? "Height" : "Width";
                    std::string def_type = is_row ? "RowDefinition" : "ColumnDefinition";
                    std::string coll = is_row ? "RowDefinitions" : "ColumnDefinitions";
                    size_t pos = 0;
                    while (pos < defs.size())
                    {
                        auto apos = defs.find(dim + "=", pos);
                        if (apos == std::string::npos) break;
                        auto q1 = defs.find('"', apos + dim.size() + 1);
                        if (q1 == std::string::npos) break;
                        auto q2 = defs.find('"', q1 + 1);
                        if (q2 == std::string::npos) break;
                        std::string val = defs.substr(q1 + 1, q2 - q1 - 1);
                        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
                        // Create definition, configure, then append (avoids typed GetAt)
                        std::string def_var = def_type + "_" + std::to_string(var_counter++);
                        out << indent << "auto " << def_var << " = Microsoft::UI::Xaml::Controls::" << def_type << "();\n";
                        std::string val_lo = val;
                        for (auto& c : val_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (!val.empty() && val.back() == '*') {
                            // Star sizing: "*" -> 1*, "2*" -> 2*
                            std::string num = val.substr(0, val.size() - 1);
                            if (num.empty()) num = "1";
                            out << indent << def_var << "." << dim << "(winrt::Microsoft::UI::Xaml::GridLength(" << num << ", winrt::Microsoft::UI::Xaml::GridUnitType::Star));\n";
                        }
                        else if (val_lo == "auto")
                            out << indent << def_var << "." << dim << "(winrt::Microsoft::UI::Xaml::GridLength(0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto));\n";
                        else
                            out << indent << def_var << "." << dim << "(winrt::Microsoft::UI::Xaml::GridLength(" << val << ", winrt::Microsoft::UI::Xaml::GridUnitType::Pixel));\n";
                        out << indent << var_name << "." << coll << "().Append(" << def_var << ");\n";
                        pos = q2 + 1;
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
        // Skip ALL properties on component types
        if (is_app_or_unknown_type(n.control)) continue;
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
            // Documents types (Paragraph, RichTextBlock, etc.): .Text(val) -> Inlines().Append(Run(val))
            {
                std::string lo_ctrl = n.control;
                for (auto& c : lo_ctrl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string lo_pk = pk;
                for (auto& c : lo_pk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                bool is_docs_type = (lo_ctrl == "paragraph" || lo_ctrl == "span" || lo_ctrl == "hyperlink" || lo_ctrl == "bold" || lo_ctrl == "italic" || lo_ctrl == "underline" || lo_ctrl == "richtextblock" || lo_ctrl == "textblock");
                if (is_docs_type && lo_pk == "text")
                {
                    std::string text_val = pv;
                    if (text_val.size() >= 2 && text_val.front() == '"' && text_val.back() == '"')
                        text_val = text_val.substr(1, text_val.size() - 2);
                    // Collapse embedded newlines
                    { std::string c; c.reserve(text_val.size());
                      for (char ch : text_val) { if (ch=='\n'||ch=='\r') c+=' '; else c+=ch; } text_val = c; }
                    // Resolve StaticResource markup extensions in text runs:
                    // drop the markup (resource text is theme-owned and cannot
                    // be concatenated into a compile-time literal here).
                    { auto sr = text_val.find("{StaticResource ");
                      if (sr != std::string::npos) {
                          auto end = text_val.find('}', sr);
                          if (end != std::string::npos)
                              text_val.erase(sr, end - sr + 1);
                      } }
                    out << indent << "{\n";
                    out << indent << "    auto _run = winrt::Microsoft::UI::Xaml::Documents::Run();\n";
                    out << indent << "    _run.Text(L\"" << escape_wstr(text_val) << "\");\n";
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
                if (is_numeric_dp(pk))
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
                                val = wstr_literal(collapsed);
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
                    std::string set_val = fix_control_enum(n.control, pk, box_if_inspectable(n.control, pk, val));
                    out << indent << var_name << "." << prop << "(" << set_val << ");\n";
                    if (!interp.empty())
                        out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(" << set_val << "); });\n";
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
                        if (is_numeric_dp(pk))
                            out << indent << var_name << "." << prop << "(" << val << ")";
                        else
                            out << indent << var_name << "." << prop << "(" << box_if_inspectable(n.control, pk, val) << ")";
                    }
                    else if (is_number_text(val))
                        // "-(50)"-style signed/parenthesized numeric expression
                        out << indent << var_name << "." << prop << "(" << val << ")";
                    else if (is_numeric_dp(pk))
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
                    out << indent << var_name << "." << prop << "(" << val << ");\n";
                    if (val.find('?') != std::string::npos || val.find("L\"") != std::string::npos)
                        out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(" << val << "); });\n";
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
                            bool skip_binding = (pk_lo_bind == "visible" || pk_lo_bind == "visibility"
                                || pk_lo_bind == "target" || pk_lo_bind == "child"
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
                                    || lo_t.find("number") != std::string::npos);
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
                                else if (pk_lo_bind2.find("alignment") != std::string::npos && pk_lo_bind2.find("horizontal") != std::string::npos)
                                    bval = "halignment_from_name(" + val + ")";
                                else if (pk_lo_bind2.find("alignment") != std::string::npos)
                                    bval = "valignment_from_name(" + val + ")";
                                if (is_numeric_dp(pk))
                                {
                                    // Numeric DP fed a string state var: no C++ conversion exists.
                                    out << indent << "// " << var_name << "." << prop << " skipped: non-numeric state var on numeric property\n";
                                    continue;
                                }
                                else
                                {
                                    out << indent << var_name << "." << prop << "(" << bval << ");\n";
                                    out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(" << bval << "); });\n";
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
                            if (!src_val.empty() && src_val[0] == '/')
                                src_val = "ms-appx:" + src_val;
                            else if (src_val.rfind("Assets/", 0) == 0)
                                src_val = "ms-appx:///" + src_val;
                            out << indent << var_name << ".Source(winrt::Microsoft::UI::Xaml::Media::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << src_val << "\")));\n";
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
                                    if (resolved.find("TitleTextBlockStyle") != std::string::npos)
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
                                            bool skip_numeric_sv3 = is_numeric_dp(pk);
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
                                            std::string set_val = fix_control_enum(n.control, pk, box_if_inspectable(n.control, pk, interp));
                                            out << indent << var_name << "." << prop << "(" << set_val << ");\n";
                                            out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << prop << "(" << set_val << "); });\n";
                                            }
                                        }
                                        else
                                        {
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
                if (g_view_type_names.count(n.control) && n.control != class_name)
                {
                    // Sibling app view: not a WinRT control - no add_<E> on the
                    // projection. Emit the forwarding call: the component view
                    // class exposes add_<E> (driver-aggregated) that stores the
                    // handler and the component fires it from its inner event.
                    std::string ev_lo = ev_prop_name;
                    for (auto& c : ev_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    out << indent << var_name << ".add_" << ev_prop_name
                        << "([this](auto const& s, auto const& e) { _fire_" << ev_lo << "(s, e); });\n";
                }
                else
                    out << indent << var_name << ".add_" << evt_prop << "([this](auto const& s, auto const& e) { " << cb_name << "(s, e); });\n";
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
            // Skip ALL raw attrs on component types (user-defined XAML dependency properties)
            if (is_app_or_unknown_type(n.control)) continue;
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
                if (rk_lo_skip == "resources" || rk_lo_skip == "visualstategroups"
                    || rk_lo_skip == "rowdefinitions" || rk_lo_skip == "columndefinitions"
                    || rk_lo_skip == "children" || rk_lo_skip == "inlines"
                    || rk_lo_skip == "menuitems" || rk_lo_skip == "tabitems"
                    || rk_lo_skip == "primarycommands" || rk_lo_skip == "secondarycommands"
                    || rk_lo_skip == "flyout" || rk_lo_skip == "styles"
                    || rk_lo_skip == "templates" || rk_lo_skip == "exittransition"
                    || rk_lo_skip == "opentransition" || rk_lo_skip == "ticks"
                    || rk_lo_skip == "linebreak" || rk_lo_skip == "containercontentchanging"
                    || rk_lo_skip == "choosingitemcontainer" || rk_lo_skip == "choosingsemanticzoomstate"
                    || rk_lo_skip == "container" || rk_lo_skip == "overrideuicommand"
                    || rk_lo_skip == "opacitymask" || rk_lo_skip == "visualstategroups") continue;
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
                    // {ThemeResource Key} -> guarded resource lookup with a
                    // codegen-baked literal fallback. Handles both the raw
                    // markup form (special keys kept rval = collapsed) and the
                    // quoted form (rval = L"{ThemeResource ...}").
                    if (rval.find("ThemeResource ") != std::string::npos)
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
                                || rk_lo_tr == "borderbrush")
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
                    if (rval.find("StaticResource ") != std::string::npos)
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
                            else if (rk_lo_sr == "itemtemplate" || rk_lo_sr == "contenttemplate"
                                || rk_lo_sr == "headertemplate" || rk_lo_sr == "footertemplate"
                                || rk_lo_sr.ends_with("template"))
                                rval = "data_template_th(L\"" + res_key + "\")";
                            else if (rk_lo_sr == "background" || rk_lo_sr == "foreground"
                                || rk_lo_sr == "fill" || rk_lo_sr == "stroke"
                                || rk_lo_sr == "borderbrush")
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
                    // Check if handler value looks like a function name (no spaces, no angle brackets)
                    if (!evt_handler.empty() && evt_handler.find(' ') == std::string::npos
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
                        if (lower_key.find("panetoggle") != std::string::npos || lower_key == "panetogglerequested")
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
                || (rval.find("make_solid_brush(") != std::string::npos);
            // XAML-specific: colons (x:Name), attached props (Grid.Row), XAML markup in values
            bool is_xaml_specific = (rk.find(':') != std::string::npos) || (rk.find('.') != std::string::npos);
            bool has_xaml_markup = !is_cxx_val && ((rval.find('<') != std::string::npos) || (rval.find('{') != std::string::npos) || (rval.find("x:Bind") != std::string::npos));
            if (is_xaml_specific || has_xaml_markup)
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
                        if (!img_val.empty() && img_val[0] == '/')
                            img_val = "ms-appx:" + img_val;
                        else if (img_val.rfind("Assets/", 0) == 0)
                            img_val = "ms-appx:///" + img_val;
                        out << indent << var_name << ".Source(winrt::Microsoft::UI::Xaml::Media::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << img_val << "\")));\n";
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
                        if (!img_val2.empty() && img_val2[0] == '/')
                            img_val2 = "ms-appx:" + img_val2;
                        else if (img_val2.rfind("Assets/", 0) == 0)
                            img_val2 = "ms-appx:///" + img_val2;
                        if (rk == "ImageSource")
                            out << indent << var_name << ".ImageSource(winrt::Microsoft::UI::Xaml::Media::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << img_val2 << "\")));\n";
                        else
                            out << indent << var_name << ".Source(winrt::Microsoft::UI::Xaml::Media::BitmapImage(winrt::Windows::Foundation::Uri(L\"" << img_val2 << "\")));\n";
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
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(" << ca << hex_to_argb(hv) << "})\n";
                                else if (hv.size() == 7)
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(" << ca << "255, " << hex_to_rgb(hv) << "})\n";
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
                                else if (is_numeric_dp(lo_rk))
                                {
                                    // numeric DP: only real numbers or valid C++ expressions may be emitted, never L"..." strings
                                    std::string num_txt = strip_wstr(emit_val);
                                    if (is_number_text(num_txt))
                                        out << indent << var_name << "." << raw_prop_name(rk) << "(" << num_txt << ");\n";
                                    else if (is_cxx_expr && interp.empty())
                                        out << indent << var_name << "." << raw_prop_name(rk) << "(" << emit_val << ");\n";
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
                                    if (emit_val.find("TitleTextBlockStyle") != std::string::npos)
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
                                else
                                    out << indent << var_name << "." << raw_prop_name(rk) << "(" << box_if_inspectable(n.control, rk, fix_control_enum(n.control, rk, emit_val)) << ")";
                                out << ";\n";
                                if (!interp.empty() && !skip_binding_push)
                                {
                                    out << indent << "m_bindings.push_back([this, " << var_name << "]() mutable { " << var_name << "." << raw_prop_name(rk) << "(" << box_if_inspectable(n.control, rk, fix_control_enum(n.control, rk, emit_val)) << "); });\n";
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
        // Children inherit the enclosing panel chain; panel-like nodes
        // extend it so conditional subtrees can attach to a real parent.
        std::vector<std::string> child_chain = parent_chain;
        if (n.control == "StackPanel" || n.control == "Grid" || n.control == "Canvas"
            || n.control == "RelativePanel" || n.control == "WrapPanel"
            || n.control == "Column" || n.control == "Row"
            || n.control == "NavigationView" || n.control == "ScrollViewer"
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
                    out << indent << var_name << ".Children().Append(" << cv << ");\n";
                else
                {
                    child_vars.push_back(std::move(cv));
                    child_ctrls.push_back(ch.control);
                }
            }
        }

        if (!child_vars.empty())
        {
            // First: route known special child types to their property setters
            // (KeyboardAccelerator → .KeyboardAccelerators(), ImageIconSource → .IconSource())
            std::vector<std::string> remaining_vars;
            for (size_t i = 0; i < child_ctrls.size(); ++i)
            {
                if (child_ctrls[i] == "KeyboardAccelerator")
                    out << indent << "inspectable_append(" << var_name << ".KeyboardAccelerators(), " << child_vars[i] << ");\n";
                else if (child_ctrls[i] == "ImageIconSource")
                    out << indent << var_name << ".IconSource(winrt::Microsoft::UI::Xaml::Controls::IconSource::wrap_borrowed(" << child_vars[i] << ".m_unknown.get()));\n";
                else
                    remaining_vars.push_back(child_vars[i]);
            }
            if (remaining_vars.empty()) { return var_name; } // all children routed
            child_vars = std::move(remaining_vars);

            // For platform controls, always use fallback composition_kind.
            // The catalog composition_kind is only for app/component types.
            std::string ck;
            bool is_platform = !g_type_ns.count(n.control) || !g_project_nss.count(g_type_ns.at(n.control));
            if (!is_platform)
                ck = catalog.composition_kind(n.control);
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
                // Single-child content controls
                else if (lo == "border" || lo == "card" || lo == "tabviewitem" || lo == "titlebar" || lo == "expander" || lo == "teachingtip" || lo == "splitview" || lo == "semanticzoom" || lo == "animatedicon" || lo == "togglesplitbutton" || lo == "textblock" || lo == "button" || lo == "togglebutton" || lo == "radiobutton" || lo == "hyperlinkbutton" || lo == "appbarbutton" || lo == "appbartogglebutton" || lo == "menuflyoutitem" || lo == "infobar" || lo == "infobadge" || lo == "tooltip" || lo == "image" || lo == "swipeitem" || lo == "slider" || lo == "scrollviewer" || lo == "scrollview" || lo == "splitbutton" || lo == "dropdownbutton" || lo == "viewbox" || lo == "pivotitem" || lo == "personpicture" || lo == "progressring" || lo == "progressbar" || lo == "loadingbutton" || lo == "colorpicker" || lo == "datepicker" || lo == "timespicker" || lo == "numberbox" || lo == "textbox" || lo == "passwordbox" || lo == "richeditbox" || lo == "ratingcontrol" || lo == "treeviewitem" || lo == "checkerboard" || lo == "refreshcontainer" || lo == "parallaxview" || lo == "mediaelement" || lo == "mediaplayerelement" || lo == "imageex" || lo == "webview2" || lo == "flyout" || lo == "toggleswitch" || lo == "infosecbutton" || lo == "switchpresenter")
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
            // ComboBox/ListView/Pivot always use .Items().Append()
            if ((n.control == "Pivot" || n.control == "ComboBox" || n.control == "ListView" || n.control == "GridView" || n.control == "FlipView") && ck != "items")
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
                                out << indent << "        if (tag == L\"Favorites\")\n";
                                out << indent << "            " << var_name << ".Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(p1.m_unknown.get()));\n";
                                out << indent << "        else\n";
                                out << indent << "            " << var_name << ".Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(p0.m_unknown.get()));\n";
                                out << indent << "    }\n";
                                out << indent << "});\n";
                            }
                        }
                    }
                    else if (child_vars.size() == 1)
                        out << indent << var_name << "." << scp << "(" << catalog.cast_child(child_vars[0], n.control) << ");\n";
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
                else
                    for (auto const& cv : child_vars)
                        out << indent << var_name << ".Children().Append(" << cv << ");\n";
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

    // Emit root nodes — MicaBackdrop becomes SystemBackdrop()
    std::vector<std::string> root_vars;
    for (auto const& rn : view.root)
    {
        if (rn.control == "MicaBackdrop") {
            if (effective_backdrop.empty()) effective_backdrop = "mica";
            continue;
        }
        std::string rv = emit_node(emit_node, rn, 0, {});
        if (!rv.empty()) root_vars.push_back(std::move(rv));
    }

    // Set the single root child as Content() (Window: the window content;
    // Page/UserControl: the page's visual root - both must attach or the
    // view renders empty).
    if (root_vars.size() == 1)
    {
        out << "        Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << root_vars[0] << ".m_unknown.get()));\n";
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
        // AppTitleBar/titleBar element must be assigned before SetTitleBar takes it
        // (a null element here would kill all titlebar dragging).
        if (is_window_class && view.extend_content_into_titlebar && (named_elements.count("AppTitleBar") || named_elements.count("titleBar")))
        {
            std::string tb_name = named_elements.count("titleBar") ? "titleBar" : "AppTitleBar";
            out << "        SetTitleBar(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << tb_name << ".m_unknown.get()));\n";
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
        out << "        Content(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(_root.m_unknown.get()));\n";
        if (is_window_class)
        {
            out << "        g_theme_state.window_id = AppWindow().Id().Value;\n";
            out << "        if (!g_theme_state.main_hwnd) g_theme_state.main_hwnd = reinterpret_cast<void*>(static_cast<std::uintptr_t>(AppWindow().Id().Value));\n";
            if (view.extend_content_into_titlebar)
                out << "        g_theme_state.is_extended_titlebar = true;\n";
            out << "        g_theme_state.root_element = static_cast<void*>(_root.m_unknown.get());\n";
            out << "        apply_app_theme(!is_current_light_theme());\n";
        }
        if (is_window_class && view.extend_content_into_titlebar && (named_elements.count("AppTitleBar") || named_elements.count("titleBar")))
        {
            std::string tb_name = named_elements.count("titleBar") ? "titleBar" : "AppTitleBar";
            out << "        SetTitleBar(winrt::Microsoft::UI::Xaml::UIElement::wrap_borrowed(" << tb_name << ".m_unknown.get()));\n";
            out << "        g_theme_state.window_id = AppWindow().Id().Value;\n";
            out << "        sync_titlebar_theme(!is_current_light_theme());\n";
            out << "        sync_window_theme(!is_current_light_theme() ? 1 : 0);\n";
        }
    }

    {
        std::string nav_view_name = has_elem("NavView") ? "NavView" : (has_elem("NavigationViewControl") ? "NavigationViewControl" : "");
        std::string frame_name = has_elem("ContentFrame") ? "ContentFrame" : (has_elem("rootFrame") ? "rootFrame" : "");
        bool is_nav_window = is_window_class && !view.nav_pages.empty() && !nav_view_name.empty() && !frame_name.empty();
        if (is_nav_window)
        {
            out << "        _navigate_to_tag(L\"Home\");\n";
            if (has_elem("Home"))
                out << "        Home.IsSelected(true);\n";
        }
    }

    out << "    }\n";

    // (Settings behavior wire-up now emitted from the page constructor,
    // right after BuildUI() - see the ctor emission above.)

    // Emit template factory functions (no load_xaml)
    for (auto const& tfr : pending_factory_funcs)
    {
        out << tfr.func_body;
    }
    } // end if (!is_app_class) — BuildUI() only for Window/UserControl

    // Navigation synthesis: when the driver supplied a tag->page map for
    // this window view, emit a Frame::Navigate dispatcher used by the
    // SelectionChanged/Loaded callbacks. Resolution is data-driven from the
    // project's own views (Tag attrs, page runtime class names) - no
    // hardcoded page names in the compiler.
    std::string nav_view_name = has_elem("NavView") ? "NavView" : (has_elem("NavigationViewControl") ? "NavigationViewControl" : "");
    std::string frame_name = has_elem("ContentFrame") ? "ContentFrame" : (has_elem("rootFrame") ? "rootFrame" : "");
    bool is_nav_window = is_window_class && !view.nav_pages.empty() && !nav_view_name.empty() && !frame_name.empty();
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
        // Direct construction dispatch: build the page view and set it as
        // the Frame's content. Zero-XAML apps have no IXamlMetadataProvider,
        // so Frame::Navigate(TypeName) cannot resolve pages; constructing
        // the page directly (via the page module's exported factory) is the
        // metadata-free equivalent with identical Frame semantics.
        out << "\n    void _navigate_to_tag(std::wstring_view tag)\n    {\n";
        out << "        winrt::IInspectable (*factory)() = nullptr;\n";
        for (auto const& [tag, page_name] : view.nav_pages)
        {
            // "Ns.Page" -> factory create_<Page>() imported from module Ns
            std::string fn = "create_";
            std::string ns_part = page_name;
            auto d = page_name.rfind('.');
            std::string page_class = (d != std::string::npos) ? page_name.substr(d + 1) : page_name;
            fn += page_class;
            out << "        if (tag == L\"" << tag << "\") factory = " << fn << ";\n";
        }
        out << "        if (!factory) return;\n";
        out << "        winrt::IInspectable page = factory();\n";
        out << "        if (page)\n";
        out << "        {\n";
        out << "            void* fe_raw = nullptr;\n";
        out << "            if (page.get_abi() && winrt::IUnknown::qi_raw(page.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::IFrameworkElement>, &fe_raw) >= 0 && fe_raw)\n";
        out << "            {\n";
        out << "                winrt::Microsoft::UI::Xaml::IFrameworkElement fe{fe_raw};\n";
        out << "                fe.put_RequestedTheme(get_global_theme());\n";
        out << "            }\n";
        out << "            " << frame_name << ".Content(page);\n";
        out << "        }\n";
        out << "    }\n";
    }

    // Settings-behavior synthesis: mirrors the original code-behind -
    // theme combo applies RequestedTheme (system/light/dark); compact
    // toggle reduces spacing + card padding across the live visual tree.
    // ResourceDictionary re-insertion is unavailable (no keyed accessors in
    // the projection, no closed-generic IMap iid), and resource lookups are
    // snapshots anyway, so compactness is applied directly element by
    // element - the same final layout state as the theme-driven original.
    if (!is_app_class && named_elements.count("ThemeComboBox"))
    {
        out << "\n    void _apply_element_theme(int idx)\n    {\n";
        out << "        g_theme_state.theme_idx = idx;\n";
        out << "        bool is_light = is_current_light_theme();\n";
        out << "        winrt::Microsoft::UI::Xaml::ElementTheme theme = is_light ? winrt::Microsoft::UI::Xaml::ElementTheme::Light : winrt::Microsoft::UI::Xaml::ElementTheme::Dark;\n";
        out << "        update_dynamic_brushes();\n";
        out << "        auto root = winrt::Microsoft::UI::Xaml::FrameworkElement(*this);\n";
        out << "        root.RequestedTheme(theme);\n";
        out << "        try\n";
        out << "        {\n";
        out << "            auto xr = root.XamlRoot();\n";
        out << "            if (xr.get_abi())\n";
        out << "            {\n";
        out << "                auto content = xr.Content();\n";
        out << "                if (content.get_abi())\n";
        out << "                {\n";
        out << "                    void* fe_raw = nullptr;\n";
        out << "                    if (winrt::IUnknown::qi_raw(content.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::IFrameworkElement>, &fe_raw) >= 0 && fe_raw)\n";
        out << "                    {\n";
        out << "                        winrt::Microsoft::UI::Xaml::IFrameworkElement fe{fe_raw};\n";
        out << "                        fe.put_RequestedTheme(theme);\n";
        out << "                    }\n";
        out << "                }\n";
        out << "            }\n";
        out << "        }\n";
        out << "        catch (...) {}\n";
        out << "        sync_titlebar_theme(!is_light);\n";
        out << "        sync_window_theme(is_light ? 0 : 1);\n";
        out << "    }\n";
        out << "\n    void _apply_compact(bool on)\n    {\n";
        out << "        try\n";
        out << "        {\n";
        out << "            g_theme_state.compact = on ? 1 : 0;\n";
        // Only reference elements this page actually names - the hardcoded
        // Card1/MainStackPanel set belongs to the Settings page, not every
        // page that carries a theme combo box.
        if (element_names.count("Card1"))
        {
            out << "            if (Card1.m_unknown.get())\n";
            out << "                Card1.Padding(winrt::Microsoft::UI::Xaml::Thickness{on ? 12.0 : 20.0, on ? 12.0 : 20.0, on ? 12.0 : 20.0, on ? 12.0 : 20.0});\n";
        }
        if (element_names.count("Card3"))
        {
            out << "            if (Card3.m_unknown.get())\n";
            out << "                Card3.Padding(winrt::Microsoft::UI::Xaml::Thickness{on ? 12.0 : 20.0, on ? 12.0 : 20.0, on ? 12.0 : 20.0, on ? 12.0 : 20.0});\n";
        }
        if (element_names.count("MainStackPanel"))
        {
            out << "            if (MainStackPanel.m_unknown.get())\n";
            out << "                MainStackPanel.Spacing(on ? 12.0 : 24.0);\n";
        }
        if (element_names.count("PageScrollViewer"))
        {
            out << "            if (PageScrollViewer.m_unknown.get())\n";
            out << "                PageScrollViewer.Padding(on ? winrt::Microsoft::UI::Xaml::Thickness{20.0, 12.0, 20.0, 20.0} : winrt::Microsoft::UI::Xaml::Thickness{40.0, 24.0, 40.0, 40.0});\n";
        }
        out << "        }\n";
        out << "        catch (...) {}\n";
        out << "    }\n";
        out << "\n    void _wire_settings_behavior()\n    {\n";
        out << "        ThemeComboBox.SelectedIndex(g_theme_state.theme_idx);\n";
        out << "        ThemeComboBox.add_SelectionChanged([this](auto const& s, auto const& e)\n";
        out << "        {\n";
        out << "            _apply_element_theme(static_cast<int>(ThemeComboBox.SelectedIndex()));\n";
        out << "        });\n";
        // CompactToggle exists only on pages that name it - a page with a
        // ThemeComboBox but no toggle must not wire a nonexistent member.
        if (element_names.count("CompactToggle"))
        {
            out << "        CompactToggle.IsOn(g_theme_state.compact != 0);\n";
            out << "        CompactToggle.add_Toggled([this](auto const& s, auto const& e)\n";
            out << "        {\n";
            out << "            _apply_compact(CompactToggle.get_IsOn());\n";
            out << "        });\n";
        }
        out << "    }\n";
    }

    // Emit event handler stubs INSIDE class body (avoids C2039 with imported type)
    std::set<std::string> extra_members; // bare C# members referenced by translated bodies
    for (auto const& cb : callbacks)
    {
        if (cb == "raw") continue;
        out << "\n    void " << cb << "(IInspectable const& sender, IInspectable const& args)\n    {\n";
        // Nav synthesis: real behavior for the callbacks the reference apps
        // wire (SelectionChanged -> tag dispatch into the Frame; Loaded ->
        // select the first menu item so the first page navigates). These run
        // BEFORE the winmd-carried statements (which stay authoritative).
        if (is_nav_window)
        {
            if (cb.find("SelectionChanged") != std::string::npos)
            {
                out << "        // synthesized: tag dispatch into " << frame_name << "\n";
                out << "        if (args)\n";
                out << "        {\n";
                out << "            void* nav_raw = args.get_abi();\n";
                out << "            void* nav_out = nullptr;\n";
                out << "            if (nav_raw && winrt::impl::query_interface(nav_raw, winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs>, &nav_out) >= 0 && nav_out)\n";
                out << "            {\n";
                out << "                auto nav_args = winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs::from_abi(nav_out);\n";
                out << "                if (nav_args.IsSettingsSelected()) { _navigate_to_tag(L\"__settings__\"); }\n";
                out << "                else\n";
                out << "                {\n";
                out << "                    winrt::IInspectable sel = nav_args.SelectedItem();\n";
                out << "                    if (sel)\n";
                out << "                    {\n";
                out << "                        void* item_out = nullptr;\n";
                out << "                        if (winrt::impl::query_interface(sel.get_abi(), winrt::iid_of<winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem>, &item_out) >= 0 && item_out)\n";
                out << "                        {\n";
                out << "                            auto item = winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem::from_abi(item_out);\n";
                out << "                            auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), winrt::hstring{});\n";
                out << "                            if (tag.empty()) tag = winrt::unbox_value_or<winrt::hstring>(item.Content(), winrt::hstring{});\n";
                out << "                            if (!tag.empty()) _navigate_to_tag(tag.to_view());\n";
                out << "                        }\n";
                out << "                    }\n";
                out << "                }\n";
                out << "            }\n";
                out << "        }\n";
            }
            if (!loaded_cb_name.empty() && cb == loaded_cb_name)
            {
                out << "        // synthesized: initial navigation (first menu item)\n";
                out << "        if (" << frame_name << ".Content() == nullptr)\n";
                out << "        {\n";
                out << "            auto items0 = " << nav_view_name << ".MenuItems();\n";
                out << "            if (items0 && items0.get_Size() > 0)\n";
                out << "            {\n";
                out << "                " << nav_view_name << ".SelectedItem(items0.GetAt(0));\n";
                out << "            }\n";
                out << "            _navigate_to_tag(L\"Home\");\n";
                out << "        }\n";
            }
        }
        if (!nav_view_name.empty() && (cb.find("PaneToggle") != std::string::npos || cb.find("panetoggle") != std::string::npos))
        {
            out << "        // synthesized: toggle navigation pane\n";
            out << "        " << nav_view_name << ".IsPaneOpen(!" << nav_view_name << ".IsPaneOpen());\n";
        }
        // Real code-behind logic carried from the winmd callback bodies
        // (language-neutral statements produced by xaml2winuic). Translate
        // the supported forms into C++; anything referencing identifiers the
        // projection does not know stays as a comment so the generated
        // module always compiles.
        auto cb_it = view.callbacks.find(cb);
        if (cb_it != view.callbacks.end())
        {
            for (auto const& raw_stmt : cb_it->second)
            {
                auto trim = [](std::string s) {
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n' || s.back() == ';')) s.pop_back();
                    return s;
                };
                std::string stmt = trim(raw_stmt);
                if (stmt.empty()) continue;
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
                    bool valid = dot != std::string::npos && dot > 0 && dot + 1 < lhs.size();
                    if (valid)
                    {
                        std::string elem = lhs.substr(0, dot);
                        std::string prop = lhs.substr(dot + 1);
                        auto is_ident = [](std::string const& s) {
                            auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
                            auto alnum = [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); };
                            if (s.empty() || !alpha(s[0])) return false;
                            for (char c : s) if (!alnum(c)) return false;
                            return true;
                        };
                        if (is_ident(elem) && is_ident(prop) && named_elements.count(elem) && !rhs.empty())
                        {
                            // Projection getters are methods: a named-element
                            // chain used as a value needs "()" on each link
                            // (MapToken.Password -> MapToken.Password(),
                            //  Tooltip.IsOpen -> Tooltip.IsOpen()).
                            std::string val_cpp = rhs;
                            {
                                std::string best_root;
                                std::size_t best_pos = std::string::npos;
                                for (auto const& [nm, t] : named_elements)
                                {
                                    std::size_t p = val_cpp.find(nm);
                                    while (p != std::string::npos)
                                    {
                                        std::size_t q = p + nm.size();
                                        if (q < val_cpp.size() && val_cpp[q] == '.')
                                        {
                                            if (best_pos == std::string::npos || p < best_pos)
                                            { best_pos = p; best_root = nm; }
                                            break;
                                        }
                                        p = val_cpp.find(nm, q);
                                    }
                                }
                                if (!best_root.empty())
                                {
                                    std::size_t i = best_pos + best_root.size();
                                    while (i < val_cpp.size() && val_cpp[i] == '.')
                                    {
                                        std::size_t j = i + 1;
                                        while (j < val_cpp.size() && (std::isalnum(static_cast<unsigned char>(val_cpp[j])) || val_cpp[j] == '_')) ++j;
                                        if (j >= val_cpp.size() || val_cpp[j] != '(')
                                        {
                                            val_cpp.insert(j, "()");
                                            i = j + 2;
                                        }
                                        else
                                            i = j + 1;
                                    }
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
                                auto eit = named_elements.find(elem);
                                std::string elem_type = (eit != named_elements.end()) ? eit->second : elem;
                                if (!is_plain_bool_ischecked(elem_type))
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
                                        if (sv.name == val_cpp) { known_state = true; break; }
                                    if (!known_state) extra_members.insert(val_cpp);
                                }
                            }
                            out << "        " << elem << "." << prop << "(" << val_cpp << ");\n";
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
            std::string ev_lo = ev;
            for (auto& c : ev_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (cb.size() >= ev.size()
                && std::tolower(static_cast<unsigned char>(cb[cb.size() - ev.size()])) == std::tolower(static_cast<unsigned char>(ev[0])))
            {
                std::string cb_tail = cb.substr(cb.size() - ev.size());
                std::string ev_pascal = ev;
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
            std::string ev_lo = ev;
            for (auto& c : ev_lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            out << "    std::vector<std::function<void(IInspectable const&, IInspectable const&)>> m_" << ev_lo << "_handlers;\n";
            out << "public:\n";
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
    if (is_window_class && !is_shell)
    {
        out << "\nexport " << cpp_class << "& get_" << class_name << "()\n";
        out << "{\n";
        out << "    static " << cpp_class << " window_view;\n";
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
        out << "    return winrt::IInspectable(get_" << class_name << "());\n";
        out << "}\n";
        // Registration hook for the local activation registry (winrt_base):
        // main.ixx imports each view module and calls this once at startup so
        // winrt::<ns>::<Class>() wrappers resolve via the in-process factory
        // instead of RoActivateInstance (app classes have no registry entry,
        // and the exe exports nothing for DllGetActivationFactory).
        out << "\n// Register the local C++ factory under the runtime class name\n";
        out << "export void register_local_factory_" << class_name << "()\n";
        out << "{\n";
        out << "    winrt::impl::register_local_factory(L\"" << ns << "." << class_name << "\", &create_" << class_name << ");\n";
        out << "}\n";
    }

    // Deterministic post-pass over the emitted module text: canonicalizes
    // enum spellings, boxes IInspectable-typed props, wraps Thickness values,
    // and comments x:Bind leftovers that reference elements this codegen can't
    // materialize. Shape-based only - never touches the prelude/helpers.
    std::string code = out.str();
    code = fixup_view_code(std::move(code));
    return code;
}

} // namespace winuic

