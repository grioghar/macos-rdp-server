#include "imsg/exporters.hpp"

#include <sstream>
#include <string>

#include "imsg/time_util.hpp"

namespace imsg {
namespace {

std::string format_when(const Message& m) {
    return m.has_date ? format_timestamp(m.date) : "unknown time";
}

std::string join(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

const char* kHtmlStyle =
    "body{font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;"
    "background:#f0f0f3;margin:0;padding:2rem;color:#1d1d1f}"
    ".conversation{max-width:720px;margin:0 auto}"
    "header h1{margin:0 0 .25rem;font-size:1.4rem}"
    "header .meta{color:#6e6e73;font-size:.85rem;margin-bottom:1.5rem}"
    ".msg{margin:.35rem 0;display:flex;flex-direction:column}"
    ".msg .bubble{display:inline-block;padding:.5rem .75rem;border-radius:1rem;"
    "max-width:75%;word-wrap:break-word;white-space:pre-wrap}"
    ".msg.them{align-items:flex-start}"
    ".msg.them .bubble{background:#e5e5ea;color:#000;border-bottom-left-radius:.25rem}"
    ".msg.me{align-items:flex-end}"
    ".msg.me .bubble{background:#0b84ff;color:#fff;border-bottom-right-radius:.25rem}"
    ".msg .info{font-size:.7rem;color:#8e8e93;margin:0 .5rem .1rem}"
    ".attachment{font-style:italic;opacity:.85}.empty{font-style:italic;opacity:.7}";

}  // namespace

bool parse_format(const std::string& name, Format& out) {
    if (name == "txt" || name == "text") { out = Format::Text; return true; }
    if (name == "json") { out = Format::Json; return true; }
    if (name == "html") { out = Format::Html; return true; }
    return false;
}

std::string extension_for(Format fmt) {
    switch (fmt) {
        case Format::Json: return "json";
        case Format::Html: return "html";
        case Format::Text: default: return "txt";
    }
}

std::string available_formats() { return "txt, json, html"; }

std::string render_text(const Chat& chat) {
    std::ostringstream os;
    os << chat.title() << "\n"
       << "Service: " << (chat.service.empty() ? "unknown" : chat.service) << "\n"
       << "Participants: "
       << (chat.participants.empty() ? "unknown" : join(chat.participants, ", ")) << "\n"
       << "Messages: " << chat.messages.size() << "\n"
       << std::string(60, '=') << "\n\n";

    for (const Message& m : chat.messages) {
        os << "[" << format_when(m) << "] " << m.sender << ":\n";
        bool wrote = false;
        if (m.has_text()) { os << "  " << m.text << "\n"; wrote = true; }
        for (const Attachment& a : m.attachments) {
            os << "  <attachment: " << a.display_name() << ">\n";
            wrote = true;
        }
        if (!wrote) os << "  (no content)\n";
        os << "\n";
    }
    return os.str();
}

std::string render_json(const Chat& chat) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"guid\": \"" << json_escape(chat.guid) << "\",\n";
    os << "  \"title\": \"" << json_escape(chat.title()) << "\",\n";
    os << "  \"chat_identifier\": \"" << json_escape(chat.chat_identifier) << "\",\n";
    os << "  \"display_name\": \"" << json_escape(chat.display_name) << "\",\n";
    os << "  \"service\": \"" << json_escape(chat.service) << "\",\n";

    os << "  \"participants\": [";
    for (std::size_t i = 0; i < chat.participants.size(); ++i) {
        if (i) os << ", ";
        os << "\"" << json_escape(chat.participants[i]) << "\"";
    }
    os << "],\n";

    os << "  \"message_count\": " << chat.messages.size() << ",\n";
    os << "  \"messages\": [";
    for (std::size_t i = 0; i < chat.messages.size(); ++i) {
        const Message& m = chat.messages[i];
        os << (i ? ",\n" : "\n");
        os << "    {\n";
        os << "      \"guid\": \"" << json_escape(m.guid) << "\",\n";
        os << "      \"date\": "
           << (m.has_date ? "\"" + json_escape(format_timestamp(m.date)) + "\"" : "null")
           << ",\n";
        os << "      \"date_read\": "
           << (m.has_date_read ? "\"" + json_escape(format_timestamp(m.date_read)) + "\""
                               : "null")
           << ",\n";
        os << "      \"is_from_me\": " << (m.is_from_me ? "true" : "false") << ",\n";
        os << "      \"sender\": \"" << json_escape(m.sender) << "\",\n";
        os << "      \"service\": \"" << json_escape(m.service) << "\",\n";
        os << "      \"text\": \"" << json_escape(m.text) << "\",\n";
        os << "      \"attachments\": [";
        for (std::size_t j = 0; j < m.attachments.size(); ++j) {
            const Attachment& a = m.attachments[j];
            os << (j ? ", " : "") << "{";
            os << "\"filename\": \"" << json_escape(a.filename) << "\", ";
            os << "\"mime_type\": \"" << json_escape(a.mime_type) << "\", ";
            os << "\"transfer_name\": \"" << json_escape(a.transfer_name) << "\", ";
            os << "\"total_bytes\": " << a.total_bytes;
            os << "}";
        }
        os << "]\n";
        os << "    }";
    }
    os << (chat.messages.empty() ? "" : "\n  ") << "]\n";
    os << "}\n";
    return os.str();
}

std::string render_html(const Chat& chat) {
    std::ostringstream os;
    const std::string title = html_escape(chat.title());
    os << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
       << "<meta charset=\"utf-8\">\n"
       << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
       << "<title>" << title << "</title>\n"
       << "<style>" << kHtmlStyle << "</style>\n</head>\n<body>\n"
       << "<div class=\"conversation\">\n<header>\n<h1>" << title << "</h1>\n"
       << "<div class=\"meta\">Service: "
       << html_escape(chat.service.empty() ? "unknown" : chat.service)
       << " &middot; Participants: "
       << html_escape(chat.participants.empty() ? "unknown" : join(chat.participants, ", "))
       << " &middot; " << chat.messages.size() << " messages</div>\n</header>\n";

    for (const Message& m : chat.messages) {
        const char* side = m.is_from_me ? "me" : "them";
        os << "<div class=\"msg " << side << "\">"
           << "<div class=\"info\">" << html_escape(m.sender) << " &middot; "
           << html_escape(format_when(m)) << "</div>"
           << "<div class=\"bubble\">";
        bool wrote = false;
        if (m.has_text()) { os << html_escape(m.text); wrote = true; }
        for (const Attachment& a : m.attachments) {
            if (wrote) os << "<br>";
            os << "<span class=\"attachment\">\xF0\x9F\x93\x8E "
               << html_escape(a.display_name()) << "</span>";
            wrote = true;
        }
        if (!wrote) os << "<span class=\"empty\">(no content)</span>";
        os << "</div></div>\n";
    }
    os << "</div>\n</body>\n</html>\n";
    return os.str();
}

std::string render(const Chat& chat, Format fmt) {
    switch (fmt) {
        case Format::Json: return render_json(chat);
        case Format::Html: return render_html(chat);
        case Format::Text: default: return render_text(chat);
    }
}

}  // namespace imsg
