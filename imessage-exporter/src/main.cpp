// Command-line entry point for imessage-exporter.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "imsg/database.hpp"
#include "imsg/exporters.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.1.0";

void print_usage(std::ostream& os) {
    os << "Usage: imessage-exporter [options]\n\n"
       << "Export macOS iMessage/SMS history to TXT, JSON, or HTML.\n\n"
       << "Options:\n"
       << "  --db PATH        Path to the Messages database\n"
       << "                   (default: $HOME/Library/Messages/chat.db)\n"
       << "  --format FMT     Output format: " << imsg::available_formats()
       << " (default: txt)\n"
       << "  --output DIR     Directory for exported files (default: ./imessage-export)\n"
       << "  --me NAME        Label for messages you sent (default: Me)\n"
       << "  --list-chats     List conversations and exit\n"
       << "  --version        Print version and exit\n"
       << "  --help           Show this help and exit\n";
}

std::string slugify(const std::string& value) {
    std::string out;
    bool last_dash = false;
    for (char ch : value) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
            last_dash = false;
        } else if (!last_dash && !out.empty()) {
            out += '-';
            last_dash = true;
        }
        if (out.size() >= 80) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

// Reads the value following a flag, erroring if it's missing.
bool take_value(int argc, char** argv, int& i, const char* flag, std::string& out) {
    if (i + 1 >= argc) {
        std::cerr << "error: " << flag << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_path = imsg::default_db_path();
    std::string format_name = "txt";
    std::string output_dir = "imessage-export";
    std::string me_label = "Me";
    bool list_chats = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else if (arg == "--version") {
            std::cout << "imessage-exporter " << kVersion << "\n";
            return 0;
        } else if (arg == "--list-chats") {
            list_chats = true;
        } else if (arg == "--db") {
            if (!take_value(argc, argv, i, "--db", db_path)) return 2;
        } else if (arg == "--format") {
            if (!take_value(argc, argv, i, "--format", format_name)) return 2;
        } else if (arg == "--output") {
            if (!take_value(argc, argv, i, "--output", output_dir)) return 2;
        } else if (arg == "--me") {
            if (!take_value(argc, argv, i, "--me", me_label)) return 2;
        } else {
            std::cerr << "error: unknown argument '" << arg << "'\n\n";
            print_usage(std::cerr);
            return 2;
        }
    }

    imsg::Format format;
    if (!imsg::parse_format(format_name, format)) {
        std::cerr << "error: unknown format '" << format_name << "'; choose from "
                  << imsg::available_formats() << "\n";
        return 2;
    }

    std::vector<imsg::Chat> chats;
    try {
        imsg::MessagesDatabase db(db_path, me_label);
        db.open();
        chats = db.load_chats();
    } catch (const imsg::DatabaseError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    // Keep only conversations that actually contain messages.
    chats.erase(std::remove_if(chats.begin(), chats.end(),
                               [](const imsg::Chat& c) { return c.messages.empty(); }),
                chats.end());

    if (chats.empty()) {
        std::cerr << "No conversations with messages were found.\n";
        return 1;
    }

    if (list_chats) {
        std::sort(chats.begin(), chats.end(),
                  [](const imsg::Chat& a, const imsg::Chat& b) {
                      return a.messages.size() > b.messages.size();
                  });
        for (const auto& c : chats) {
            std::cout << c.messages.size() << "\t" << c.title() << "\n";
        }
        return 0;
    }

    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (ec) {
        std::cerr << "error: cannot create output directory '" << output_dir
                  << "': " << ec.message() << "\n";
        return 1;
    }

    const std::string ext = imsg::extension_for(format);
    std::unordered_set<std::string> used;
    std::size_t written = 0;

    for (const auto& chat : chats) {
        std::string base = slugify(chat.title());
        if (base.empty()) base = "chat-" + std::to_string(chat.rowid);
        std::string name = base + "." + ext;
        for (int n = 2; used.count(name); ++n)
            name = base + "-" + std::to_string(n) + "." + ext;
        used.insert(name);

        fs::path path = fs::path(output_dir) / name;
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            std::cerr << "error: cannot write '" << path.string() << "'\n";
            return 1;
        }
        out << imsg::render(chat, format);
        ++written;
    }

    std::cout << "Exported " << written << " conversation(s) to " << output_dir
              << " as " << format_name << ".\n";
    return 0;
}
