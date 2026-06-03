// Read-only access to the macOS Messages (`chat.db`) database.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "imsg/models.hpp"

namespace imsg {

// Thrown when the database cannot be opened or queried.
class DatabaseError : public std::runtime_error {
   public:
    explicit DatabaseError(const std::string& what) : std::runtime_error(what) {}
};

// Default location of the live Messages database for the current user.
std::string default_db_path();

class MessagesDatabase {
   public:
    // `me_label` is used as the sender name for messages you sent.
    explicit MessagesDatabase(std::string db_path, std::string me_label = "Me");
    ~MessagesDatabase();

    MessagesDatabase(const MessagesDatabase&) = delete;
    MessagesDatabase& operator=(const MessagesDatabase&) = delete;

    // Opens the database read-only/immutable. Throws DatabaseError on failure.
    void open();
    void close();

    // Loads all conversations (with participants, messages, attachments).
    std::vector<Chat> load_chats();

   private:
    std::string db_path_;
    std::string me_label_;
    void* db_ = nullptr;  // sqlite3* (opaque to avoid leaking the header here)
};

}  // namespace imsg
