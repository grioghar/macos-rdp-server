// Core data models for exported iMessage structures.
#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace imsg {

struct Attachment {
    std::string filename;
    std::string mime_type;
    std::string transfer_name;
    long long total_bytes = 0;

    // Human-friendly name, preferring the transfer name the sender saw.
    std::string display_name() const;
};

struct Message {
    long long rowid = 0;
    std::string guid;
    std::string text;

    bool has_date = false;
    std::time_t date = 0;        // epoch seconds (local rendering done at output)
    bool has_date_read = false;
    std::time_t date_read = 0;

    bool is_from_me = false;
    std::string sender;          // handle id, or the "me" label
    std::string service;         // "iMessage" / "SMS"

    bool has_chat = false;
    long long chat_id = -1;

    std::vector<Attachment> attachments;

    bool has_text() const;
};

struct Chat {
    long long rowid = 0;
    std::string guid;
    std::string chat_identifier;
    std::string display_name;
    std::string service;
    std::vector<std::string> participants;
    std::vector<Message> messages;

    // Best human-readable name for the conversation.
    std::string title() const;
};

}  // namespace imsg
