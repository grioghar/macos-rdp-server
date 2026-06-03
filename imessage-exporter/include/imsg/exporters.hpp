// Renderers that turn a conversation into output text.
#pragma once

#include <string>
#include <vector>

#include "imsg/models.hpp"

namespace imsg {

enum class Format { Text, Json, Html };

// Parses a format name ("txt"/"json"/"html"); returns false if unknown.
bool parse_format(const std::string& name, Format& out);

// File extension for a format ("txt"/"json"/"html").
std::string extension_for(Format fmt);

// Comma-separated list of supported format names, for help text.
std::string available_formats();

std::string render_text(const Chat& chat);
std::string render_json(const Chat& chat);
std::string render_html(const Chat& chat);

// Dispatches to the renderer for `fmt`.
std::string render(const Chat& chat, Format fmt);

}  // namespace imsg
