#include "imsg/models.hpp"

#include <algorithm>
#include <cctype>

namespace imsg {

std::string Attachment::display_name() const {
    if (!transfer_name.empty()) return transfer_name;
    if (!filename.empty()) return filename;
    return "(attachment)";
}

bool Message::has_text() const {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
}

std::string Chat::title() const {
    if (!display_name.empty()) return display_name;
    if (!participants.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < participants.size(); ++i) {
            if (i) joined += ", ";
            joined += participants[i];
        }
        return joined;
    }
    if (!chat_identifier.empty()) return chat_identifier;
    if (!guid.empty()) return guid;
    return "chat-" + std::to_string(rowid);
}

}  // namespace imsg
