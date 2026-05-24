// usage: fixspec-gen-fields <spec.xml>... [-o fields.hpp] [-go groups.hpp]
// Reads QuickFIX-format XML; FIX 5.0 needs two (FIX50SP2.xml + FIXT11.xml).
// Dups merge; conflicts emit a warning on stderr (name clash -> alias,
// type/delimiter mismatch -> first wins).

#include <pugixml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Field {
    int tag = 0;
    std::string name;
    std::string type;
    std::vector<std::string> aliases;
};

struct MessageType {
    std::string name;
    std::string value;
    std::vector<std::string> aliases;
};

struct GroupDef {
    std::string count_name;
    std::string first_name;
};

// Alternate delimiter for a NoXxx, with the messages it appeared in. The
// first entry across alternates wins for compile-time dispatch.
struct GroupAlternate {
    std::string first_name;
    std::vector<std::string> messages;
};

std::string right_pad(std::size_t width, char pad, std::string_view text) {
    std::string out(text);
    if (out.size() < width)
        out.append(width - out.size(), pad);
    return out;
}

// QuickFIX uppercases its type names; map to FIX spec CamelCase.
std::string normalize_type(std::string_view qf) {
    static std::map<std::string_view, char const*> const table = {
        {"INT", "int"},
        {"LENGTH", "Length"},
        {"NUMINGROUP", "NumInGroup"},
        {"SEQNUM", "SeqNum"},
        {"TAGNUM", "TagNum"},
        {"FLOAT", "float"},
        {"QTY", "Qty"},
        {"PRICE", "Price"},
        {"PRICEOFFSET", "PriceOffset"},
        {"AMT", "Amt"},
        {"PERCENTAGE", "Percentage"},
        {"CHAR", "char"},
        {"BOOLEAN", "Boolean"},
        {"STRING", "String"},
        {"MULTIPLECHARVALUE", "MultipleCharValue"},
        {"MULTIPLESTRINGVALUE", "MultipleStringValue"},
        {"COUNTRY", "Country"},
        {"CURRENCY", "Currency"},
        {"EXCHANGE", "Exchange"},
        {"MONTHYEAR", "MonthYear"},
        {"UTCTIMESTAMP", "UTCTimestamp"},
        {"UTCTIMEONLY", "UTCTimeOnly"},
        {"UTCDATEONLY", "UTCDateOnly"},
        {"LOCALMKTDATE", "LocalMktDate"},
        {"LOCALMKTTIME", "LocalMktTime"},
        {"TZTIMEONLY", "TZTimeOnly"},
        {"TZTIMESTAMP", "TZTimestamp"},
        {"DATA", "data"},
        {"XMLDATA", "XMLData"},
        {"LANGUAGE", "Language"},
        {"XID", "XID"},
        {"XIDREF", "XIDREF"},
    };
    if (auto it = table.find(qf); it != table.end())
        return it->second;
    return std::string(qf);
}

void load_fields(pugi::xml_document const& doc, char const* source, std::map<int, Field>& out) {
    for (auto f : doc.child("fix").child("fields").children("field")) {
        char const* num = f.attribute("number").value();
        char const* name = f.attribute("name").value();
        char const* type = f.attribute("type").value();
        if (!num[0] || !name[0] || !type[0])
            continue;
        int tag = std::atoi(num);
        if (tag <= 0)
            continue;
        std::string normalized = normalize_type(type);
        auto [it, inserted] = out.try_emplace(tag, Field{tag, name, normalized, {}});
        if (inserted)
            continue;
        Field& existing = it->second;
        if (existing.name != name &&
            std::find(existing.aliases.begin(), existing.aliases.end(), name) ==
                existing.aliases.end()) {
            std::cerr << "warning: tag " << tag << " has alias " << name << " (primary "
                      << existing.name << ", in " << source << ")\n";
            existing.aliases.emplace_back(name);
        }
        if (existing.type != normalized) {
            std::cerr << "warning: tag " << tag << " (" << existing.name << ") type "
                      << existing.type << " vs " << normalized << " in " << source << ", keeping "
                      << existing.type << "\n";
        }
    }
}

void load_messages(pugi::xml_document const& doc,
                   char const* source,
                   std::vector<MessageType>& out,
                   std::unordered_map<std::string, std::size_t>& seen_by_value) {
    for (auto m : doc.child("fix").child("messages").children("message")) {
        char const* name = m.attribute("name").value();
        char const* value = m.attribute("msgtype").value();
        if (!name[0] || !value[0])
            continue;
        if (auto it = seen_by_value.find(value); it != seen_by_value.end()) {
            MessageType& existing = out[it->second];
            if (existing.name != name &&
                std::find(existing.aliases.begin(), existing.aliases.end(), name) ==
                    existing.aliases.end()) {
                std::cerr << "warning: msgtype '" << value << "' has alias " << name << " (primary "
                          << existing.name << ", in " << source << ")\n";
                existing.aliases.emplace_back(name);
            }
            continue;
        }
        seen_by_value.emplace(value, out.size());
        out.push_back(MessageType{name, value, {}});
    }
}

void resolve_component_first(pugi::xml_node fix_root,
                             std::unordered_map<std::string, std::string>& out) {
    for (auto c : fix_root.child("components").children("component")) {
        std::string name = c.attribute("name").value();
        if (name.empty())
            continue;
        for (auto child : c.children()) {
            std::string_view tag = child.name();
            if (tag == "field" || tag == "group") {
                std::string first = child.attribute("name").value();
                if (!first.empty())
                    out.try_emplace(std::move(name), std::move(first));
                break;
            }
            if (tag == "component") {
                std::string nested = child.attribute("name").value();
                if (!nested.empty())
                    out.try_emplace(std::move(name), std::move(nested));
                break;
            }
        }
    }
}

using ConflictTable = std::map<std::string, std::vector<GroupAlternate>>;

void collect_groups(pugi::xml_node node,
                    std::string const& message_context,
                    std::map<std::string, GroupDef>& dedup,
                    ConflictTable& conflicts) {
    for (auto child : node.children()) {
        std::string_view tag = child.name();
        if (tag == "group") {
            std::string count_name = child.attribute("name").value();
            std::string first_name;
            for (auto inner : child.children()) {
                std::string_view itag = inner.name();
                if (itag == "field" || itag == "component" || itag == "group") {
                    first_name = inner.attribute("name").value();
                    break;
                }
            }
            if (!count_name.empty() && !first_name.empty()) {
                dedup.try_emplace(count_name, GroupDef{count_name, first_name});
                auto& alts = conflicts[count_name];
                auto a = std::find_if(alts.begin(), alts.end(), [&](GroupAlternate const& g) {
                    return g.first_name == first_name;
                });
                if (a == alts.end()) {
                    alts.push_back(GroupAlternate{first_name, {}});
                    a = std::prev(alts.end());
                }
                if (!message_context.empty() &&
                    std::find(a->messages.begin(), a->messages.end(), message_context) ==
                        a->messages.end()) {
                    a->messages.push_back(message_context);
                }
            }
        }
        collect_groups(child, message_context, dedup, conflicts);
    }
}

void collect_all_groups(pugi::xml_node fix_root,
                        std::map<std::string, GroupDef>& dedup,
                        ConflictTable& conflicts) {
    for (auto m : fix_root.child("messages").children("message")) {
        std::string mname = m.attribute("name").value();
        collect_groups(m, mname, dedup, conflicts);
    }
    // Each component is its own context to attribute reuse conflicts.
    for (auto c : fix_root.child("components").children("component")) {
        std::string cname = c.attribute("name").value();
        collect_groups(c, cname, dedup, conflicts);
    }
    collect_groups(fix_root.child("header"), "header", dedup, conflicts);
    collect_groups(fix_root.child("trailer"), "trailer", dedup, conflicts);
}

// Leaf field name reached by chasing component_first; empty on cycle or depth exhaustion (warns).
std::string resolve_first_field(std::string name,
                                std::unordered_map<std::string, std::string> const& component_first,
                                std::string const& group_name) {
    constexpr int kMaxComponentDepth = 32;
    std::unordered_set<std::string> visited;
    std::vector<std::string> chain;
    chain.push_back(name);
    for (int depth = 0; depth < kMaxComponentDepth; ++depth) {
        auto it = component_first.find(name);
        if (it == component_first.end())
            return name;
        if (!visited.insert(name).second) {
            std::fprintf(stderr,
                         "warning: group %s: cyclic component reference (%s)\n",
                         group_name.c_str(),
                         [&] {
                             std::string s;
                             for (std::size_t i = 0; i < chain.size(); ++i) {
                                 if (i)
                                     s += " -> ";
                                 s += chain[i];
                             }
                             return s;
                         }()
                             .c_str());
            return {};
        }
        name = it->second;
        chain.push_back(name);
    }
    std::fprintf(stderr,
                 "warning: group %s: component depth limit (%d) exceeded resolving delimiter\n",
                 group_name.c_str(),
                 kMaxComponentDepth);
    return {};
}

void load_groups(std::vector<pugi::xml_document const*> const& docs,
                 std::vector<GroupDef>& out,
                 ConflictTable& out_conflicts) {
    std::unordered_map<std::string, std::string> component_first;
    for (auto* d : docs)
        resolve_component_first(d->child("fix"), component_first);

    std::map<std::string, GroupDef> dedup;
    for (auto* d : docs)
        collect_all_groups(d->child("fix"), dedup, out_conflicts);

    // Resolve component refs in each delimiter; drop the group on cycle/depth.
    // Resolve alternates too so the conflict comment shows real field names.
    out.reserve(dedup.size());
    for (auto& [_, g] : dedup) {
        std::string resolved = resolve_first_field(g.first_name, component_first, g.count_name);
        if (resolved.empty()) {
            out_conflicts.erase(g.count_name);
            continue;
        }
        g.first_name = std::move(resolved);
        out.push_back(g);
    }
    for (auto& [cname, alts] : out_conflicts) {
        for (auto& a : alts) {
            std::string r = resolve_first_field(a.first_name, component_first, cname);
            if (!r.empty())
                a.first_name = std::move(r);
        }
        // Collapse alternates that resolved to the same leaf; preserve order.
        std::vector<GroupAlternate> merged;
        for (auto& a : alts) {
            auto it = std::find_if(merged.begin(), merged.end(), [&](GroupAlternate const& g) {
                return g.first_name == a.first_name;
            });
            if (it == merged.end()) {
                merged.push_back(std::move(a));
            } else {
                for (auto& m : a.messages)
                    if (std::find(it->messages.begin(), it->messages.end(), m) == it->messages.end())
                        it->messages.push_back(std::move(m));
            }
        }
        alts = std::move(merged);
    }

    // Drop entries with a single delimiter (no conflict).
    for (auto it = out_conflicts.begin(); it != out_conflicts.end();) {
        if (it->second.size() < 2)
            it = out_conflicts.erase(it);
        else {
            std::cerr << "warning: group " << it->first << " has " << it->second.size()
                      << " delimiters (";
            for (std::size_t i = 0; i < it->second.size(); ++i) {
                if (i)
                    std::cerr << ", ";
                std::cerr << it->second[i].first_name;
            }
            std::cerr << "), compile-time dispatch uses " << it->second.front().first_name
                      << "; runtime overload available for the others\n";
            ++it;
        }
    }
}

bool is_data_length(Field const& f) {
    if (f.type != "Length")
        return false;
    // 9 = BodyLength (frame), 383 = MaxMessageSize (session limit); see upstream #44.
    if (f.tag == 9 || f.tag == 383)
        return false;
    return true;
}

void emit_fields(std::ostream& os,
                 std::map<int, Field> const& fields,
                 std::vector<MessageType> const& messages) {
    os << "// Generated by utils/fixspec-gen-fields. Do not edit.\n"
       << "\n"
       << "#pragma once\n"
       << "\n"
       << "namespace hffix {\n"
       << "\n"
       << "namespace tag {\n"
       << "enum {\n";

    auto last = fields.empty() ? fields.end() : std::prev(fields.end());
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        Field const& f = it->second;
        bool is_last = it == last && f.aliases.empty();
        os << right_pad(50, ' ', f.name) << " = " << f.tag << (is_last ? "" : ",") << " /*!< "
           << f.tag << " (" << f.type << ") */\n";
        for (std::size_t i = 0; i < f.aliases.size(); ++i) {
            bool alias_last = it == last && i + 1 == f.aliases.size();
            os << right_pad(50, ' ', f.aliases[i]) << " = " << f.tag << (alias_last ? "" : ",")
               << " /*!< " << f.tag << " alias of " << f.name << " */\n";
        }
    }

    os << "};\n"
       << "} // namespace tag\n"
       << "\n"
       << "namespace {\n"
       << "inline constexpr int length_fields[] = {\n";

    std::vector<Field const*> lengths;
    for (auto const& [_, f] : fields) {
        if (is_data_length(f))
            lengths.push_back(&f);
    }
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        Field const& f = *lengths[i];
        bool is_last = i + 1 == lengths.size();
        std::string entry = "tag::" + f.name;
        if (!is_last)
            entry += ",";
        os << right_pad(35, ' ', entry) << " // " << f.tag << "\n";
    }
    os << "};\n"
       << "}\n"
       << "\n"
       << "template <typename AssociativeContainer>\n"
       << "void dictionary_init_field(AssociativeContainer& dictionary) {\n";

    for (auto const& [_, f] : fields) {
        os << right_pad(50, ' ', "dictionary[tag::" + f.name + "]") << " = "
           << "\"" << f.name << "\";\n";
    }

    os << "}\n"
       << "\n"
       << "namespace msg_type {\n";

    for (auto const& m : messages) {
        os << "static constexpr char const* const " << right_pad(40, ' ', m.name) << " = "
           << "\"" << m.value << "\";\n";
        for (auto const& alias : m.aliases) {
            os << "static constexpr char const* const " << right_pad(40, ' ', alias) << " = "
               << m.name << "; // alias\n";
        }
    }

    os << "} // namespace msg_type\n"
       << "\n"
       << "template <typename AssociativeContainer>\n"
       << "void dictionary_init_message(AssociativeContainer& dictionary) {\n";

    for (auto const& m : messages) {
        os << right_pad(25, ' ', "dictionary[\"" + m.value + "\"]") << " = "
           << "\"" << m.name << "\";\n";
    }

    os << "}\n"
       << "} // namespace hffix\n";
}

void emit_groups(std::ostream& os,
                 std::vector<GroupDef> const& groups,
                 ConflictTable const& conflicts) {
    os << "// Generated by utils/fixspec-gen-fields. Do not edit.\n"
       << "//\n"
       << "// One HFFIX_REGISTER_GROUP per repeating group; maps NoXxx to its\n"
       << "// delimiter (first field) for compile-time dispatch via reader.group<NoXxx>().\n"
       << "\n"
       << "#pragma once\n"
       << "\n"
       << "#include <hffix.hpp>\n"
       << "\n";
    for (auto const& g : groups) {
        auto cit = conflicts.find(g.count_name);
        if (cit != conflicts.end()) {
            os << "// " << g.count_name
               << ": first-tag conflicts across contexts (messages/components).\n";
            for (auto const& alt : cit->second) {
                os << "//   " << right_pad(28, ' ', alt.first_name) << "(in ";
                for (std::size_t i = 0; i < alt.messages.size(); ++i) {
                    if (i)
                        os << ", ";
                    os << alt.messages[i];
                }
                if (alt.messages.empty())
                    os << "components";
                os << ")\n";
            }
            os << "// Compile-time dispatch picks the first-seen (" << g.first_name << ").\n"
               << "// For other contexts (messages/components), use reader.group(tag::"
               << g.count_name << ", tag::<delimiter>) runtime overload.\n";
        }
        os << "HFFIX_REGISTER_GROUP(" << g.count_name << ", " << g.first_name << ");\n";
    }
}

bool load_doc(char const* path, pugi::xml_document& doc) {
    auto r = doc.load_file(path);
    if (!r) {
        std::cerr << "failed to parse " << path << ": " << r.description() << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<char const*> input_paths;
    char const* fields_out = nullptr;
    char const* groups_out = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            fields_out = argv[++i];
        } else if (arg == "-go" && i + 1 < argc) {
            groups_out = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        } else {
            input_paths.push_back(argv[i]);
        }
    }

    if (input_paths.empty()) {
        std::cerr << "usage: " << argv[0]
                  << " <spec.xml> [<spec2.xml> ...] [-o <fields-out>] [-go <groups-out>]\n";
        return 2;
    }

    std::vector<pugi::xml_document> docs(input_paths.size());
    for (std::size_t i = 0; i < input_paths.size(); ++i) {
        if (!load_doc(input_paths[i], docs[i]))
            return 1;
    }

    std::map<int, Field> fields;
    std::vector<MessageType> messages;
    std::unordered_map<std::string, std::size_t> seen_msg;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        load_fields(docs[i], input_paths[i], fields);
        load_messages(docs[i], input_paths[i], messages, seen_msg);
    }

    if (fields_out) {
        std::ofstream out(fields_out, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "cannot open " << fields_out << " for writing\n";
            return 1;
        }
        emit_fields(out, fields, messages);
    } else if (!groups_out) {
        emit_fields(std::cout, fields, messages);
    }

    if (groups_out) {
        std::vector<pugi::xml_document const*> doc_ptrs;
        doc_ptrs.reserve(docs.size());
        for (auto& d : docs)
            doc_ptrs.push_back(&d);
        std::vector<GroupDef> groups;
        ConflictTable conflicts;
        load_groups(doc_ptrs, groups, conflicts);

        std::ofstream out(groups_out, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "cannot open " << groups_out << " for writing\n";
            return 1;
        }
        emit_groups(out, groups, conflicts);
        std::cerr << "wrote " << groups.size() << " HFFIX_REGISTER_GROUP entries to " << groups_out
                  << "\n";
    }
    return 0;
}
