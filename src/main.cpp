#include "xdl/db.h"
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#else
#include <unistd.h>
#endif
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
// XDL  interactive + batch CLI
//
//  Usage:
//    xdl <dbfile>
//    echo "insert 1 25 alice" | xdl mydb.xdl
//
//  Commands:
//    insert <id> <age> <name>
//    get    <id>
//    scan   [--min-age N] [--max-age N] [--name-prefix STR]
//    stats
//    checkpoint
//    help
//    quit / exit
// ─────────────────────────────────────────────────────────────────────────────

static void print_row(const xdl::Row& r) {
    std::cout
        << "id="   << std::setw(6) << r.id
        << "  age=" << std::setw(4) << r.age
        << "  name=" << r.name_str()
        << "\n";
}

static void print_help() {
    std::cout <<
        "Commands:\n"
        "  insert <id> <age> <name>              Insert a new row\n"
        "  get    <id>                           Retrieve row by id\n"
        "  scan   [--min-age N] [--max-age N]\n"
        "         [--name-prefix STR]            Scan all rows (optional filter)\n"
        "  stats                                 Show database statistics\n"
        "  checkpoint                            Flush dirty pages to disk\n"
        "  help                                  Show this help\n"
        "  quit | exit                           Close and exit\n";
}

static void handle_insert(xdl::DB& db, std::istringstream& ss) {
    uint32_t id, age;
    std::string name;
    if (!(ss >> id >> age >> name)) {
        std::cerr << "Usage: insert <id> <age> <name>\n";
        return;
    }
    db.insert(id, age, name);
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
        print_row(row);
    } else {
        std::cerr << "Not found: " << id << "\n";
    }
}

static void handle_scan(xdl::DB& db, std::istringstream& ss) {
    xdl::ScanFilter f;
    std::string tok;
    while (ss >> tok) {
        if (tok == "--min-age")       { uint32_t v; ss >> v; f.min_age = v; }
        else if (tok == "--max-age")  { uint32_t v; ss >> v; f.max_age = v; }
        else if (tok == "--name-prefix") { std::string p; ss >> p; f.name_prefix = p; }
        else { std::cerr << "Unknown scan option: " << tok << "\n"; }
    }

    size_t count = 0;
    db.scan([&](const xdl::Row& r){
        print_row(r);
        ++count;
    }, f);
    std::cout << count << " row(s)\n";
}

static void handle_stats(xdl::DB& db) {
    xdl::Stats s = db.stats();
    std::cout
        << "rows          : " << s.row_count       << "\n"
        << "pages         : " << s.page_count       << "\n"
        << "cache capacity: " << s.cache_capacity   << " pages\n"
        << "file size     : " << s.file_size_bytes  << " bytes\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: xdl <database_file>\n";
        return 1;
    }

    xdl::DB db(argv[1]);
    try {
        db.open();
    } catch (const std::exception& e) {
        std::cerr << "Failed to open database: " << e.what() << "\n";
        return 1;
    }

    bool interactive = isatty(fileno(stdin));
    if (interactive) {
        std::cout << "XDL v1  — " << argv[1] << "\n";
        std::cout << "Type 'help' for commands.\n";
    }

    std::string line;
    while (true) {
        if (interactive) std::cout << "xdl> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        // Trim leading whitespace
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
            else if (cmd == "stats")      handle_stats(db);
            else if (cmd == "checkpoint") { db.checkpoint(); std::cout << "Checkpoint done\n"; }
            else if (cmd == "help")       print_help();
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