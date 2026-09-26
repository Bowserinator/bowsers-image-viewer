#pragma once
#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "color.hpp"
#include "str.hpp"

namespace butil {

// ============================================================
// Argument metadata
// ============================================================

struct ArgMeta {
    std::string m_name;
    std::string m_help;
    std::string m_metavar;
    enum class Action { STORE, STORE_TRUE, STORE_FALSE, COUNT } m_action = Action::STORE;
    int m_nargs                                                          = 1;
    bool m_required                                                      = false;
    std::string m_default_val;
    std::string m_dest;
    std::vector<std::string> m_choices;
    std::string m_short_name;
};

struct Arg {
    std::string m_name;
    std::string m_help;
    std::string m_metavar;
    ArgMeta::Action m_action = ArgMeta::Action::STORE;
    int m_nargs              = 1;
    bool m_required          = false;
    std::string m_default_val;
    std::string m_dest;
    std::vector<std::string> m_choices;
    std::string m_short_name;

    Arg() = default;

    Arg(std::string n, std::string h = "") : m_name(std::move(n)), m_help(std::move(h)) {}

    Arg& help(std::string h) {
        m_help = std::move(h);
        return *this;
    }

    Arg& metavar(std::string m) {
        m_metavar = std::move(m);
        return *this;
    }

    Arg& choices(std::initializer_list<std::string> cs) {
        m_choices.assign(cs.begin(), cs.end());
        return *this;
    }

    Arg& action(ArgMeta::Action a) {
        m_action = a;
        return *this;
    }

    Arg& nargs(int n) {
        m_nargs = n;
        return *this;
    }

    Arg& required() {
        m_required = true;
        return *this;
    }

    Arg& required(bool r) {
        m_required = r;
        return *this;
    }

    Arg& dest(std::string d) {
        m_dest = std::move(d);
        return *this;
    }

    Arg& meta(std::string d) { return dest(std::move(d)); }  // alias for dest()

    Arg& short_name(std::string s) {
        m_short_name = std::move(s);
        return *this;
    }

    Arg& default_val(std::string dv) {
        m_default_val = std::move(dv);
        return *this;
    }
};

// Section for grouping arguments in help
struct Section {
    std::string title;
    std::vector<Arg> args;
};

// Forward declarations (needed before Parser, since Parser's inline
// methods call these)
inline std::string make_help(
    const std::vector<Arg>& args, const std::string& prog, const std::string& desc, bool include_help);
inline std::string make_help(
    const std::vector<Section>& sections, const std::string& prog = "prog", const std::string& desc = "");
inline std::optional<std::map<std::string, std::string>> parse_args(
    int argc, char** argv, const std::vector<Arg>& args, const std::string& prog = "", const std::string& desc = "");
inline std::optional<std::map<std::string, std::string>> parse_args(int argc,
    char** argv,
    const std::vector<Section>& sections,
    const std::string& prog = "",
    const std::string& desc = "");

// ============================================================
// Parser – central API using add_section / add_argument
// ============================================================

class Parser {
public:
    Parser(const std::string& prog = "", const std::string& desc = "") : m_prog(prog), m_desc(desc) {
        // start with an unnamed default section
        m_sections.push_back({"", {}});
        m_cur_section = &m_sections.back();
    }

    // Add a new named section (returns *this for chaining)
    Parser& add_section(const std::string& title) {
        // finalize previous section (if it has a title or already contains args)
        if (!m_cur_section->title.empty() || !m_cur_section->args.empty())
            m_sections.push_back({title, {}});
        else
            m_cur_section->title = title;  // reuse the first placeholder

        m_cur_section = &m_sections.back();
        return *this;
    }

    // Add an argument to the current section
    Parser& add_argument(const Arg& a) {
        m_cur_section->args.push_back(a);
        return *this;
    }

    // Return a flat vector of Args (useful for internal helpers like parse_known_args)
    std::vector<Arg> get_flat_args() const {
        std::vector<Arg> flat;
        for (const auto& sec : m_sections)
            flat.insert(flat.end(), sec.args.begin(), sec.args.end());
        return flat;
    }

    // Generate help text (uses the section-aware make_help)
    std::string help() const { return make_help(m_sections, m_prog, m_desc); }

    // Parse arguments (delegates to existing parse_args overload)
    std::optional<std::map<std::string, std::string>> parse(int argc, char** argv) const {
        return parse_args(argc, argv, m_sections, m_prog, m_desc);
    }

private:
    std::string m_prog;
    std::string m_desc;
    std::vector<Section> m_sections;
    Section* m_cur_section;
};

// Parse with sections (for grouped help)
inline std::optional<std::map<std::string, std::string>> parse_args(
    int argc, char** argv, const std::vector<Section>& sections, const std::string& prog, const std::string& desc) {

    // Flatten sections for parsing
    std::vector<Arg> args;
    for (const auto& sec : sections)
        for (const auto& a : sec.args)
            args.push_back(a);

    // Check for help flag
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << make_help(sections, prog, desc);
            return std::nullopt;
        }
    }

    return parse_args(argc, argv, args, prog, desc);
}

// Returns true if `value` matches one of the entries in `choices`
// (or if `choices` is empty, meaning "no restriction").
inline bool value_in_choices(const std::string& value, const std::vector<std::string>& choices) {
    if (choices.empty())
        return true;
    for (const auto& c : choices)
        if (c == value)
            return true;
    return false;
}

// Core: parse argc/argv into optional map of string values
inline std::optional<std::map<std::string, std::string>> parse_args(
    int argc, char** argv, const std::vector<Arg>& args, const std::string& prog, const std::string& desc) {
    // Check for help flag
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << make_help(args, prog, desc, true);
            return std::nullopt;
        }
    }

    std::map<std::string, std::string> vals;
    std::set<std::string> provided;

    // Seed defaults (keyed by dest, falling back to name)
    for (const auto& a : args) {
        if (!a.m_default_val.empty()) {
            std::string key = a.m_dest.empty() ? a.m_name : a.m_dest;
            vals[key]       = a.m_default_val;
        }
    }

    // Parse argv
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.empty())
            continue;
        if (arg == "--")
            break;

        if (arg[0] == '-' && arg.size() > 1) {
            bool matched = false;
            for (const auto& a : args) {
                if (arg == a.m_name || (!a.m_short_name.empty() && arg == a.m_short_name)) {
                    provided.insert(a.m_name);
                    std::string key = a.m_dest.empty() ? a.m_name : a.m_dest;

                    // Action type is checked BEFORE looking at the next
                    // token, so flags that never take a value (STORE_TRUE /
                    // STORE_FALSE / COUNT) don't accidentally swallow the
                    // next positional argument as their "value".
                    if (a.m_action == ArgMeta::Action::STORE_TRUE) {
                        vals[key] = "1";
                    } else if (a.m_action == ArgMeta::Action::STORE_FALSE) {
                        vals[key] = "0";
                    } else if (a.m_action == ArgMeta::Action::COUNT) {
                        int count = 0;
                        if (auto it = vals.find(key); it != vals.end())
                            if (auto c = str_to<int>(it->second))
                                count = *c;
                        vals[key] = std::to_string(count + 1);
                    } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                        std::string value = argv[++i];
                        if (!value_in_choices(value, a.m_choices)) {
                            std::cerr << "error: argument " << a.m_name << ": invalid choice: '" << value
                                      << "' (choose from " << butil::join(a.m_choices, ", ") << ")\n";
                            return std::nullopt;
                        }
                        vals[key] = value;
                    } else {
                        vals[key] = "1";
                    }
                    matched = true;
                    break;
                }
            }
            (void)matched;
        } else {
            // Accumulate positional args instead of overwriting each other.
            // NOTE: joined with '\n', not ' ' -- a space is a valid (and common)
            // character inside a single argv token (e.g. a file path), so
            // space-joining would make it impossible to split the tokens back
            // apart unambiguously. '\n' can never appear inside a single argv
            // element, so it's a safe separator. See main.cpp for the matching split.
            if (!vals["args"].empty())
                vals["args"] += "\n";
            vals["args"] += arg;
        }
    }

    // Enforce required args: an arg is satisfied if it was explicitly
    // provided on the command line, or if it has a default value (already
    // seeded above). Anything else is a hard error.
    std::vector<std::string> missing_required;
    for (const auto& a : args)
        if (a.m_required && !provided.count(a.m_name) && a.m_default_val.empty())
            missing_required.push_back(a.m_name);
    if (!missing_required.empty()) {
        for (const auto& m : missing_required)
            std::cerr << "error: the following required argument is missing: " << m << "\n";
        return std::nullopt;
    }

    return vals;
}

// parse_known_args: reorders argv so unknown args are at the start (after argv[0])
// returns pair: (known_args_map, num_unknown)
inline std::pair<std::optional<std::map<std::string, std::string>>, int> parse_known_args(
    int argc, char** argv, const std::vector<Arg>& args) {

    std::vector<char*> unknown;
    std::vector<char*> known;
    known.reserve(argc);
    unknown.reserve(argc);

    known.push_back(argv[0]);

    std::set<std::string> known_flags;
    for (const auto& a : args) {
        known_flags.insert(a.m_name);
        if (!a.m_short_name.empty())
            known_flags.insert(a.m_short_name);
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--") {
            for (int j = i + 1; j < argc; ++j)
                unknown.push_back(argv[j]);
            break;
        }
        if (arg[0] == '-' && arg.size() > 1) {
            if (known_flags.contains(arg)) {
                known.push_back(argv[i]);
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    known.push_back(argv[i + 1]);
                    ++i;
                }
            } else
                unknown.push_back(argv[i]);
        } else
            unknown.push_back(argv[i]);
    }

    int unknown_count = static_cast<int>(unknown.size());
    for (size_t i = 0; i < unknown.size(); ++i)
        argv[i + 1] = unknown[i];
    for (size_t i = 1; i < known.size(); ++i)
        argv[unknown_count + i] = known[i];
    argv[unknown_count + known.size()] = nullptr;

    int new_argc = unknown_count + static_cast<int>(known.size());
    auto result  = parse_args(new_argc, argv, args);

    return {result, unknown_count};
}

// Overload parse_known_args for Parser (uses internal flat args)
inline std::pair<std::optional<std::map<std::string, std::string>>, int> parse_known_args(
    int argc, char** argv, const Parser& p) {
    return parse_known_args(argc, argv, p.get_flat_args());
}

// ============================================================
// Help rendering helpers (shared by both make_help overloads so
// the per-argument formatting logic only lives in one place)
// ============================================================

namespace detail {

inline std::string arg_display(const Arg& a) {
    std::string display = a.m_name;
    if (!a.m_short_name.empty())
        display += ", " + a.m_short_name;
    if (!a.m_metavar.empty())
        display += " " + a.m_metavar;
    return display;
}

// Appends the required args (unbracketed) then the optional args
// (bracketed) to the usage line.
inline void append_usage_args(std::ostringstream& ss, const std::vector<Arg>& args) {
    for (const auto& a : args)
        if (a.m_required)
            ss << " " << arg_display(a);
    for (const auto& a : args)
        if (!a.m_required)
            ss << " [" << arg_display(a) << "]";
}

// Writes the full "Usage: ..." header line (bold/underlined "Usage:")
inline void write_usage_line(std::ostringstream& ss, const std::string& prog, const std::vector<Arg>& flat_args) {
    if (USE_COLOR)
        ss << BOLD << UNDERLINE;
    ss << "Usage:" << RESET;
    ss << " " << prog;
    append_usage_args(ss, flat_args);
    ss << "\n";
}

inline std::size_t max_display_width(const std::vector<Arg>& args, std::size_t min_width = 4) {
    std::size_t max_n = min_width;
    for (const auto& a : args)
        max_n = std::max(max_n, arg_display(a).size());
    return max_n;
}

// Writes a single aligned "  --flag, -f METAVAR   help text (choices: ...) (required) (Default: x)" line
inline void write_arg_line(std::ostringstream& ss, const Arg& a, std::size_t max_n) {
    std::string display = arg_display(a);
    ss << "  ";
    if (USE_COLOR)
        ss << FG_YELLOW;
    ss << display;
    if (USE_COLOR)
        ss << RESET;
    ss << std::string(max_n - display.size() + 2, ' ');
    ss << "  " << a.m_help;
    if (!a.m_choices.empty())
        ss << " (choices: " << butil::join(a.m_choices, ", ") << ")";
    if (a.m_action == ArgMeta::Action::COUNT)
        ss << " (repeatable)";
    if (a.m_required)
        ss << " (required)";
    if (!a.m_default_val.empty())
        ss << " (Default: " << a.m_default_val << ")";
    ss << "\n";
}

}  // namespace detail

// Help string generation (colored) - flat argument list
inline std::string make_help(
    const std::vector<Arg>& args, const std::string& prog, const std::string& desc, bool include_help) {

    std::ostringstream ss;
    detail::write_usage_line(ss, prog, args);
    if (!desc.empty())
        ss << desc << "\n";

    // Display help option if enabled
    if (include_help)
        ss << "  -h, --help     Show help\n";

    // Render arguments, aligned to the widest display string
    std::size_t max_n = detail::max_display_width(args);
    for (const auto& a : args)
        detail::write_arg_line(ss, a, max_n);
    return ss.str();
}

// Help with sections
inline std::string make_help(const std::vector<Section>& sections, const std::string& prog, const std::string& desc) {

    std::ostringstream ss;

    std::vector<Arg> flat;
    for (const auto& sec : sections)
        flat.insert(flat.end(), sec.args.begin(), sec.args.end());
    detail::write_usage_line(ss, prog, flat);

    if (!desc.empty())
        ss << desc << "\n";
    ss << "\n";

    // Render sections
    for (const auto& sec : sections) {
        if (!sec.title.empty()) {
            if (USE_COLOR)
                ss << BOLD << UNDERLINE;
            ss << sec.title << RESET;
            ss << "\n";
        }

        std::size_t max_n = detail::max_display_width(sec.args);
        for (const auto& a : sec.args)
            detail::write_arg_line(ss, a, max_n);
    }
    return ss.str();
}

}  // namespace butil