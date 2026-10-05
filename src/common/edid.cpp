#include "brodisplays/edid.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace brodisplays {

namespace {

constexpr uint8_t kEdidHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

bool verify_checksum(const uint8_t* block) {
    uint32_t sum = 0;
    for (size_t i = 0; i < 128; ++i) {
        sum += block[i];
    }
    return (sum & 0xFF) == 0;
}

std::string clean_string(const char* src, size_t max_len) {
    std::string s;
    for (size_t i = 0; i < max_len; ++i) {
        char c = src[i];
        if (c == 0x0A || c == 0x00) break;
        if (c >= 0x20 && c <= 0x7E) {
            s.push_back(c);
        }
    }
    // Trim trailing spaces
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s;
}

} // namespace

std::string decode_edid_manufacturer_id(uint16_t id_be) {
    // 2 bytes in big-endian:
    // Bits: 14-10: char 1, 9-5: char 2, 4-0: char 3 (each 1-26 -> 'A'-'Z')
    uint8_t c1 = static_cast<uint8_t>((id_be >> 10) & 0x1F);
    uint8_t c2 = static_cast<uint8_t>((id_be >> 5) & 0x1F);
    uint8_t c3 = static_cast<uint8_t>(id_be & 0x1F);

    std::string res;
    if (c1 >= 1 && c1 <= 26) res.push_back(static_cast<char>('A' + c1 - 1));
    if (c2 >= 1 && c2 <= 26) res.push_back(static_cast<char>('A' + c2 - 1));
    if (c3 >= 1 && c3 <= 26) res.push_back(static_cast<char>('A' + c3 - 1));
    return res;
}

bool parse_edid(const uint8_t* data, size_t size, EdidInfo& out_info) {
    if (!data || size < 128) return false;

    // Check header
    if (std::memcmp(data, kEdidHeader, 8) != 0) {
        return false;
    }

    // Verify 128-byte base checksum
    if (!verify_checksum(data)) {
        return false;
    }

    out_info.raw_bytes.assign(data, data + size);

    // Manufacturer ID: bytes 8 and 9 (big-endian 16-bit word)
    uint16_t mfg_raw = static_cast<uint16_t>((data[8] << 8) | data[9]);
    out_info.manufacturer_id = decode_edid_manufacturer_id(mfg_raw);

    // Product Code: bytes 10 and 11 (little-endian)
    out_info.product_code = static_cast<uint16_t>(data[10] | (data[11] << 8));

    // Serial Number: bytes 12..15 (little-endian)
    out_info.serial_number = static_cast<uint32_t>(
        data[12] | (data[13] << 8) | (data[14] << 16) | (data[15] << 24));

    // Dimensions in cm
    out_info.width_cm = data[0x15];
    out_info.height_cm = data[0x16];

    // Detailed descriptors at 0x36, 0x48, 0x5A, 0x6C (4 descriptors, 18 bytes each)
    for (size_t d = 0; d < 4; ++d) {
        const uint8_t* desc = data + 0x36 + (d * 18);
        if (desc[0] == 0 && desc[1] == 0) {
            // Display descriptor
            uint8_t tag = desc[3];
            if (tag == 0xFC) {
                // Monitor Name
                out_info.monitor_name = clean_string(reinterpret_cast<const char*>(desc + 5), 13);
            }
        }
    }

    return true;
}

std::vector<DisplayMode> extract_edid_modes(const uint8_t* data, size_t size) {
    std::vector<DisplayMode> modes;
    if (!data || size < 128) return modes;
    if (std::memcmp(data, kEdidHeader, 8) != 0) return modes;

    auto add_mode = [&](uint32_t w, uint32_t h, double rr) {
        if (w == 0 || h == 0 || rr <= 0.0) return;
        // Check for duplicate
        for (const auto& m : modes) {
            if (m.width == w && m.height == h && std::abs(m.refresh_rate - rr) < 0.5) {
                return;
            }
        }
        modes.push_back(DisplayMode{w, h, std::round(rr * 100.0) / 100.0});
    };

    // Established Timings I & II
    uint8_t t1 = data[0x23];
    uint8_t t2 = data[0x24];
    if (t1 & 0x80) add_mode(800, 600, 60.0);
    if (t1 & 0x40) add_mode(800, 600, 56.0);
    if (t1 & 0x20) add_mode(640, 480, 75.0);
    if (t1 & 0x10) add_mode(640, 480, 72.0);
    if (t1 & 0x08) add_mode(640, 480, 67.0);
    if (t1 & 0x04) add_mode(640, 480, 60.0);
    if (t1 & 0x02) add_mode(720, 400, 88.0);
    if (t1 & 0x01) add_mode(720, 400, 70.0);

    if (t2 & 0x80) add_mode(1280, 1024, 75.0);
    if (t2 & 0x40) add_mode(1024, 768, 75.0);
    if (t2 & 0x20) add_mode(1024, 768, 70.0);
    if (t2 & 0x10) add_mode(1024, 768, 60.0);
    if (t2 & 0x08) add_mode(1024, 768, 87.0);
    if (t2 & 0x04) add_mode(832, 624, 75.0);
    if (t2 & 0x02) add_mode(800, 600, 75.0);
    if (t2 & 0x01) add_mode(800, 600, 72.0);

    // Standard Timings (8 slots of 2 bytes at 0x26)
    for (size_t i = 0; i < 8; ++i) {
        uint8_t b1 = data[0x26 + (i * 2)];
        uint8_t b2 = data[0x26 + (i * 2) + 1];
        if (b1 == 0x01 && b2 == 0x01) continue; // Unused slot

        uint32_t w = (static_cast<uint32_t>(b1) + 31) * 8;
        uint8_t aspect_bits = (b2 >> 6) & 0x03;
        double rr = static_cast<double>((b2 & 0x3F) + 60);

        uint32_t h = 0;
        switch (aspect_bits) {
            case 0: h = (w * 10) / 16; break; // 16:10
            case 1: h = (w * 3) / 4;   break; // 4:3
            case 2: h = (w * 4) / 5;   break; // 5:4
            case 3: h = (w * 9) / 16;  break; // 16:9
        }
        add_mode(w, h, rr);
    }

    // Detailed Timing Descriptors (4 descriptors at 0x36)
    for (size_t d = 0; d < 4; ++d) {
        const uint8_t* desc = data + 0x36 + (d * 18);
        uint32_t pixel_clock = static_cast<uint32_t>(desc[0] | (desc[1] << 8)) * 10000;
        if (pixel_clock == 0) continue; // Display descriptor, not a timing

        uint32_t h_active = desc[2] | ((desc[4] >> 4) << 8);
        uint32_t h_blank = desc[3] | ((desc[4] & 0x0F) << 8);
        uint32_t v_active = desc[5] | ((desc[7] >> 4) << 8);
        uint32_t v_blank = desc[6] | ((desc[7] & 0x0F) << 8);

        uint32_t h_total = h_active + h_blank;
        uint32_t v_total = v_active + v_blank;

        if (h_total > 0 && v_total > 0) {
            double rr = static_cast<double>(pixel_clock) / static_cast<double>(h_total * v_total);
            add_mode(h_active, v_active, rr);
        }
    }

    return modes;
}

} // namespace brodisplays
