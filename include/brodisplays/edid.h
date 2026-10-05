#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "brodisplays/types.h"

namespace brodisplays {

// Parses 128-byte base EDID (and any CEA/CTA-861 extensions) into an EdidInfo structure.
// Returns true if a valid EDID header (00 FF FF FF FF FF FF 00) and checksum were found.
bool parse_edid(const uint8_t* data, size_t size, EdidInfo& out_info);

// Decodes the 2-byte big-endian PNP manufacturer code into a 3-letter ASCII string (e.g. "DEL", "SAM").
std::string decode_edid_manufacturer_id(uint16_t id_be);

// Extracts standard and detailed timing modes present in the EDID block.
std::vector<DisplayMode> extract_edid_modes(const uint8_t* data, size_t size);

} // namespace brodisplays
