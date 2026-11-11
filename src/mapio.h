#pragma once
#include <string>
#include "document.h"

// Reads and writes the text "iwmap 4" format. Anything the editor does not understand
// (patches, layer lines, brush attributes) is preserved verbatim.
bool loadMap(const std::string& path, Document& doc, std::string& error);
bool saveMap(const std::string& path, const Document& doc, std::string& error);
bool parseMapText(const std::string& text, Document& doc, std::string& error);
std::string writeMapText(const Document& doc);
