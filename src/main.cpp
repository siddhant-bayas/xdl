#include "xdl/db.h"
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
// XDL  interactive + batch CLI  (dynamic-schema edition)
//
//  Usage:
//    xdl <dbfile> [--schema "field:type,..."]
//
//  Schema syntax:
//    Comma-separated list of  <name:type>  pairs.
//    Supported types: uint32, string, float32, bool, int64
//    Default schema when --schema is omitted: age:uint32,name:string
//
//  Commands:
//    insert <id> <field1_value> <field2_value> ...
//    get    <id>
//    scan   [--<field> <value>]  [--min-<field> N]  [--max-<field> N]
//           [--prefix-<field> STR]
//    index  <field_name>       Create a secondary index
//    stats
//    checkpoint
//    schema                    Show current schema
//    help
//    quit / exit
// ─────────────────────────────────────────────────────────────────────────────

static xdl::Schema parse_schema(const std::string& spec) {
    xdl::Schema schema;
    std::istringstream ss(spec);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto colon = token.find(':');
        if (colon == std::string::npos)
            throw std::invalid_argument("Bad schema token (expected name:type): " + token);
        std::string name = token.substr(0, colon);
        std::string type = token.substr(colon + 1);
        try {
            schema.fields.emplace_back(name, xdl::parse_field_type(type));
        } catch (...) {
            throw std::invalid_argument("Unknown type '" + type + "'. Use uint32, string, float32, bool, or int64.");
        }
    }
    return schema;
}

static void print_row(const xdl::Row& row, const xdl::Schema& schema) {
    std::cout << "id=" << std::setw(6) << row.id;

    for (size_t i = 0; i < schema.fields.size(); ++i) {
        std::cout << "  " << schema.fields[i].name << "=";

        if (i >= row.fields.size()) {
            std::cout << "NULL";
            continue;
        }

        switch (schema.fields[i].type) {
        case xdl::FieldType::UINT32:
            std::cout << std::setw(6) << row.get_uint32(i);
            break;
        case xdl::FieldType::STRING:
            std::cout << std::left << std::setw(12) << row.get_string(i) << std::right;
            break;
        case xdl::FieldType::FLOAT32:
            std::cout << std::setw(8) << std::fixed << std::setprecision(2) << row.get_float32(i);
            break;
        case xdl::FieldType::BOOL:
            std::cout << std::setw(6) << (row.get_bool(i) ? "true" : "false");
            break;
        case xdl::FieldType::INT64:
            std::cout << std::setw(10) << row.get_int64(i);
            break;
        }
    }

    std::cout << "\n";
}

static void print_schema(const xdl::Schema& schema) {
    std::cout << "Schema v" << schema.ver.version
              << " (" << schema.fields.size() << " field(s)):\n";
    for (const auto& f : schema.fields) {
        std::cout << "  " << f.name << " : " << xdl::field_type_name(f.type) << "\n";
    }
}

static void print_help(const xdl::Schema& schema) {
    std::cout <<
        "Commands:\n"
        "  insert <id> <v1> [<v2> ...]      Insert a row (values in schema order)\n"
        "  get    <id>                       Retrieve row by id\n"
        "  scan   [--<field> <val>]\n"
        "         [--min-<field> N]\n"
        "         [--max-<field> N]\n"
        "         [--prefix-<field> STR]     Scan rows with optional field filters\n"
        "  index  <field_name>               Create a secondary index on a field\n"
        "  schema                            Display current schema\n"
        "  stats                             Show database statistics\n"
        "  checkpoint                        Flush dirty pages to disk\n"
        "  help                              Show this help\n"
        "  quit | exit                       Close and exit\n"
        "\n";
    print_schema(schema);
}

static xdl::FieldValue parse_value(const std::string& tok, xdl::FieldType type) {
    switch (type) {
    case xdl::FieldType::UINT32:
        return xdl::FieldValue{static_cast<uint32_t>(std::stoul(tok))};
    case xdl::FieldType::INT64:
        return xdl::FieldValue{static_cast<int64_t>(std::stoll(tok))};
    case xdl::FieldType::FLOAT32:
        return xdl::FieldValue{std::stof(tok)};
    case xdl::FieldType::BOOL: {
        bool b = (tok == "true" || tok == "1" || tok == "yes");
        return xdl::FieldValue{b};
    }
    case xdl::FieldType::STRING:
        return xdl::FieldValue{tok};
    }
    throw std::invalid_argument("Cannot parse value for field type");
}

static void handle_insert(xdl::DB& db, std::istringstream& ss) {
    const xdl::Schema& schema = db.schema();
    uint32_t id;
    if (!(ss >> id)) {
        std::cerr << "Usage: insert <id> <field_values...>\n";
        return;
    }

    std::vector<xdl::FieldValue> fv;
    for (const auto& fd : schema.fields) {
        std::string tok;
        if (!(ss >> tok)) {
            std::cerr << "Missing value for field '" << fd.name << "'\n";
            return;
        }
        try {
            fv.push_back(parse_value(tok, fd.type));
        } catch (...) {
            std::cerr << "Field '" << fd.name << "' expects " << xdl::field_type_name(fd.type)
                      << "; got: " << tok << "\n";
            return;
        }
    }

    db.insert(xdl::Row{id, std::move(fv)});
    std::cout << "Inserted id=" << id << "\n";
}

static void handle_get(xdl::DB& db, std::istringstream& ss) {
    uint32_t id;
    if (!(ss >> id)) {
        std::cerr << "Usage: get <id>\n";
        return;
    }
    xdl::Row row;
    if (db.try_get(id, row)) {
        print_row(row, db.schema());
    } else {
        std::cerr << "Not found: " << id << "\n";
    }
}

static void handle_scan(xdl::DB& db, std::istringstream& ss) {
    const xdl::Schema& schema = db.schema();
    xdl::ScanFilter f;
    std::string tok;

    while (ss >> tok) {
        if (tok.substr(0, 2) != "--") {
            std::cerr << "Unknown scan token: " << tok << "\n";
            continue;
        }
        std::string opt = tok.substr(2);

        if (opt.substr(0, 4) == "min-") {
            std::string field = opt.substr(4);
            int idx = schema.field_index(field);
            if (idx < 0) { std::string dummy; ss >> dummy; continue; }
            auto ft = schema.fields[static_cast<size_t>(idx)].type;
            std::string vstr; ss >> vstr;
            auto v = parse_value(vstr, ft);
            if (ft == xdl::FieldType::UINT32) f.min(field, std::get<uint32_t>(v));
            else if (ft == xdl::FieldType::INT64) f.min(field, std::get<int64_t>(v));
            else if (ft == xdl::FieldType::FLOAT32) f.min(field, std::get<float>(v));
        }
        else if (opt.substr(0, 4) == "max-") {
            std::string field = opt.substr(4);
            int idx = schema.field_index(field);
            if (idx < 0) { std::string dummy; ss >> dummy; continue; }
            auto ft = schema.fields[static_cast<size_t>(idx)].type;
            std::string vstr; ss >> vstr;
            auto v = parse_value(vstr, ft);
            if (ft == xdl::FieldType::UINT32) f.max(field, std::get<uint32_t>(v));
            else if (ft == xdl::FieldType::INT64) f.max(field, std::get<int64_t>(v));
            else if (ft == xdl::FieldType::FLOAT32) f.max(field, std::get<float>(v));
        }
        else if (opt.substr(0, 7) == "prefix-") {
            std::string field = opt.substr(7);
            std::string v; ss >> v;
            f.starts(field, v);
        }
        else {
            std::string field = opt;
            int idx = schema.field_index(field);
            if (idx < 0) {
                std::cerr << "Unknown field: " << field << "\n";
                std::string dummy; ss >> dummy;
                continue;
            }
            auto ft = schema.fields[static_cast<size_t>(idx)].type;
            std::string vstr; ss >> vstr;
            auto v = parse_value(vstr, ft);
            if (ft == xdl::FieldType::UINT32) f.eq(field, std::get<uint32_t>(v));
            else if (ft == xdl::FieldType::INT64) f.eq(field, std::get<int64_t>(v));
            else if (ft == xdl::FieldType::FLOAT32) f.eq(field, std::get<float>(v));
            else if (ft == xdl::FieldType::BOOL) f.eq(field, std::get<bool>(v));
            else f.eq(field, std::get<std::string>(v));
        }
    }

    size_t count = 0;
    db.scan([&](const xdl::Row& r){
        print_row(r, schema);
        ++count;
    }, f);
    std::cout << count << " row(s)\n";
}

static void handle_stats(xdl::DB& db) {
    xdl::Stats s = db.stats();
    std::cout
        << "rows          : " << s.row_count      << "\n"
        << "pages         : " << s.page_count      << "\n"
        << "cache capacity: " << s.cache_capacity  << " pages\n"
        << "file size     : " << s.file_size_bytes << " bytes\n"
        << "schema version: " << s.schema_version  << "\n"
        << "WAL active    : " << (s.wal_active ? "yes" : "no") << "\n"
        << "indexes       : " << s.secondary_index_count << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: xdl <database_file> [--schema \"field:type,...\"]\n"
                  << "  Example: xdl mydb.xdl --schema \"id:uint32,score:uint32,tag:string\"\n"
                  << "  Default schema: age:uint32,name:string\n";
        return 1;
    }

    xdl::Schema schema;
    for (int i = 2; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--schema" && i + 1 < argc) {
            try {
                schema = parse_schema(argv[++i]);
            } catch (const std::exception& e) {
                std::cerr << "Schema error: " << e.what() << "\n";
                return 1;
            }
        }
    }

    xdl::DB db(argv[1], std::move(schema));
    try {
        db.open();
    } catch (const std::exception& e) {
        std::cerr << "Failed to open database: " << e.what() << "\n";
        return 1;
    }

    bool interactive = isatty(fileno(stdin));
    if (interactive) {
        std::cout << "XDL v1  — " << argv[1] << "\n";
        print_schema(db.schema());
        std::cout << "Type 'help' for commands.\n";
    }

    std::string line;
    while (true) {
        if (interactive) std::cout << "xdl> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        try {
            if      (cmd == "insert")     handle_insert(db, ss);
            else if (cmd == "get")        handle_get(db, ss);
            else if (cmd == "scan")       handle_scan(db, ss);
            else if (cmd == "index") {
                std::string field; ss >> field;
                db.create_index(field);
                std::cout << "Index created on '" << field << "'\n";
            }
            else if (cmd == "schema")     print_schema(db.schema());
            else if (cmd == "stats")      handle_stats(db);
            else if (cmd == "checkpoint") { db.checkpoint(); std::cout << "Checkpoint done\n"; }
            else if (cmd == "help")       print_help(db.schema());
            else if (cmd == "quit" || cmd == "exit") break;
            else    std::cerr << "Unknown command: " << cmd << "  (try 'help')\n";
        } catch (const xdl::DuplicateKeyError& e) {
            std::cerr << "Error: " << e.what() << "\n";
        } catch (const xdl::NotFoundError& e) {
            std::cerr << "Error: " << e.what() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
    }

    try { db.close(); } catch (...) {}
    return 0;
}
