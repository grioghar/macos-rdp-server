#include "imsg/database.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "imsg/attributed_body.hpp"
#include "imsg/time_util.hpp"

namespace imsg {
namespace {

sqlite3* as_db(void* p) { return static_cast<sqlite3*>(p); }

// Reads a TEXT column, returning "" for NULL.
std::string column_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* t = sqlite3_column_text(stmt, col);
    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
}

// Collects the set of column names present in `table` (for schema differences
// across macOS versions).
std::unordered_set<std::string> table_columns(sqlite3* db, const std::string& table) {
    std::unordered_set<std::string> cols;
    std::string sql = "PRAGMA table_info(" + table + ")";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return cols;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cols.insert(column_text(stmt, 1));  // column 1 = name
    }
    sqlite3_finalize(stmt);
    return cols;
}

}  // namespace

std::string default_db_path() {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : "";
    return base + "/Library/Messages/chat.db";
}

MessagesDatabase::MessagesDatabase(std::string db_path, std::string me_label)
    : db_path_(std::move(db_path)), me_label_(std::move(me_label)) {}

MessagesDatabase::~MessagesDatabase() { close(); }

void MessagesDatabase::open() {
    // Open read-only and immutable so the live database is never modified.
    std::string uri = "file:" + db_path_ + "?mode=ro&immutable=1";
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(uri.c_str(), &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
        sqlite3_close(db);
        throw DatabaseError("cannot open " + db_path_ + ": " + msg);
    }
    db_ = db;
}

void MessagesDatabase::close() {
    if (db_) {
        sqlite3_close(as_db(db_));
        db_ = nullptr;
    }
}

std::vector<Chat> MessagesDatabase::load_chats() {
    if (!db_) throw DatabaseError("database is not open");
    sqlite3* db = as_db(db_);

    std::vector<Chat> chats;
    std::unordered_map<long long, std::size_t> index;  // chat ROWID -> chats[]

    // --- chats ---------------------------------------------------------
    {
        auto cols = table_columns(db, "chat");
        std::string service = cols.count("service_name") ? "service_name"
                                                         : "NULL AS service_name";
        std::string sql =
            "SELECT ROWID, guid, chat_identifier, display_name, " + service +
            " FROM chat ORDER BY ROWID";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw DatabaseError(std::string("query chats: ") + sqlite3_errmsg(db));
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Chat c;
            c.rowid = sqlite3_column_int64(stmt, 0);
            c.guid = column_text(stmt, 1);
            c.chat_identifier = column_text(stmt, 2);
            c.display_name = column_text(stmt, 3);
            c.service = column_text(stmt, 4);
            index[c.rowid] = chats.size();
            chats.push_back(std::move(c));
        }
        sqlite3_finalize(stmt);
    }

    // --- participants --------------------------------------------------
    {
        const char* sql =
            "SELECT chj.chat_id, h.id FROM chat_handle_join chj "
            "JOIN handle h ON chj.handle_id = h.ROWID ORDER BY chj.chat_id";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                long long chat_id = sqlite3_column_int64(stmt, 0);
                auto it = index.find(chat_id);
                if (it != index.end())
                    chats[it->second].participants.push_back(column_text(stmt, 1));
            }
            sqlite3_finalize(stmt);
        }
    }

    // --- attachments (message ROWID -> attachments) --------------------
    std::unordered_map<long long, std::vector<Attachment>> attachments;
    {
        const char* sql =
            "SELECT maj.message_id, a.filename, a.mime_type, a.transfer_name, "
            "a.total_bytes FROM message_attachment_join maj "
            "JOIN attachment a ON maj.attachment_id = a.ROWID";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Attachment a;
                long long msg_id = sqlite3_column_int64(stmt, 0);
                a.filename = column_text(stmt, 1);
                a.mime_type = column_text(stmt, 2);
                a.transfer_name = column_text(stmt, 3);
                a.total_bytes = sqlite3_column_int64(stmt, 4);
                attachments[msg_id].push_back(std::move(a));
            }
            sqlite3_finalize(stmt);
        }
    }

    // --- messages ------------------------------------------------------
    {
        auto cols = table_columns(db, "message");
        std::string attributed =
            cols.count("attributedBody") ? "m.attributedBody" : "NULL";
        std::string service = cols.count("service") ? "m.service" : "NULL";
        std::string sql =
            "SELECT m.ROWID, m.guid, m.text, " + attributed +
            ", m.date, m.date_read, m.is_from_me, " + service +
            ", h.id, cmj.chat_id FROM message m "
            "LEFT JOIN handle h ON m.handle_id = h.ROWID "
            "LEFT JOIN chat_message_join cmj ON m.ROWID = cmj.message_id "
            "ORDER BY m.date ASC, m.ROWID ASC";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw DatabaseError(std::string("query messages: ") + sqlite3_errmsg(db));

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Message m;
            m.rowid = sqlite3_column_int64(stmt, 0);
            m.guid = column_text(stmt, 1);
            m.text = column_text(stmt, 2);

            if (m.text.empty() && sqlite3_column_type(stmt, 3) == SQLITE_BLOB) {
                const auto* blob =
                    static_cast<const unsigned char*>(sqlite3_column_blob(stmt, 3));
                int n = sqlite3_column_bytes(stmt, 3);
                if (blob && n > 0)
                    m.text = decode_attributed_body(blob, static_cast<std::size_t>(n));
            }

            std::time_t when;
            if (apple_time_to_epoch(sqlite3_column_int64(stmt, 4), when)) {
                m.has_date = true;
                m.date = when;
            }
            if (apple_time_to_epoch(sqlite3_column_int64(stmt, 5), when)) {
                m.has_date_read = true;
                m.date_read = when;
            }

            m.is_from_me = sqlite3_column_int(stmt, 6) != 0;
            m.service = column_text(stmt, 7);
            std::string handle = column_text(stmt, 8);
            m.sender = m.is_from_me ? me_label_ : (handle.empty() ? "Unknown" : handle);

            if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
                m.has_chat = true;
                m.chat_id = sqlite3_column_int64(stmt, 9);
            }

            auto at = attachments.find(m.rowid);
            if (at != attachments.end()) m.attachments = at->second;

            if (m.has_chat) {
                auto it = index.find(m.chat_id);
                if (it != index.end())
                    chats[it->second].messages.push_back(std::move(m));
            }
        }
        sqlite3_finalize(stmt);
    }

    return chats;
}

}  // namespace imsg
