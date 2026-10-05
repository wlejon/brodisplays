#include "check.h"
#include "brodisplays/edid.h"

#include <numeric>
#include <vector>

namespace {

// Helper to compute and set valid EDID checksum on the 128th byte
void fix_edid_checksum(uint8_t* block) {
    uint32_t sum = 0;
    for (size_t i = 0; i < 127; ++i) {
        sum += block[i];
    }
    block[127] = static_cast<uint8_t>((256 - (sum & 0xFF)) & 0xFF);
}

std::vector<uint8_t> create_synthetic_edid(
    const std::string& mfg_id,
    uint16_t product_code,
    const std::string& monitor_name,
    uint8_t width_cm,
    uint8_t height_cm) {
    std::vector<uint8_t> edid(128, 0);

    // Header: 00 FF FF FF FF FF FF 00
    edid[0] = 0x00;
    edid[1] = 0xFF;
    edid[2] = 0xFF;
    edid[3] = 0xFF;
    edid[4] = 0xFF;
    edid[5] = 0xFF;
    edid[6] = 0xFF;
    edid[7] = 0x00;

    // Manufacturer ID: 3 letters (5 bits each)
    if (mfg_id.size() >= 3) {
        uint16_t c1 = (mfg_id[0] - 'A' + 1) & 0x1F;
        uint16_t c2 = (mfg_id[1] - 'A' + 1) & 0x1F;
        uint16_t c3 = (mfg_id[2] - 'A' + 1) & 0x1F;
        uint16_t mfg_word = static_cast<uint16_t>((c1 << 10) | (c2 << 5) | c3);
        edid[8] = static_cast<uint8_t>((mfg_word >> 8) & 0xFF);
        edid[9] = static_cast<uint8_t>(mfg_word & 0xFF);
    }

    // Product Code
    edid[10] = static_cast<uint8_t>(product_code & 0xFF);
    edid[11] = static_cast<uint8_t>((product_code >> 8) & 0xFF);

    // Serial number (0x12345678)
    edid[12] = 0x78;
    edid[13] = 0x56;
    edid[14] = 0x34;
    edid[15] = 0x12;

    // Week and Year (Week 10, Year 2024 -> 2024 - 1990 = 34 = 0x22)
    edid[16] = 10;
    edid[17] = 34;

    // EDID Version 1.4
    edid[18] = 1;
    edid[19] = 4;

    // Video input: Digital, DisplayPort/HDMI
    edid[20] = 0xA5;

    // Physical dimensions
    edid[21] = width_cm;
    edid[22] = height_cm;

    // Gamma 2.2 (value = (gamma * 100) - 100 = 120 = 0x78)
    edid[23] = 120;

    // Established timings (1024x768@60, 800x600@60, 640x480@60)
    edid[0x23] = 0x24; // 640x480@75, 640x480@60
    edid[0x24] = 0x10; // 1024x768@60

    // Detailed Timing Descriptor 1 (1920x1080 @ 60Hz) at 0x36
    // Pixel clock = 148.50 MHz = 14850 * 10kHz = 0x3A02
    edid[0x36] = 0x02;
    edid[0x37] = 0x3A;
    // H active = 1920 (0x780), H blank = 280 (0x118)
    edid[0x38] = 0x80;
    edid[0x39] = 0x18;
    edid[0x3A] = 0x71; // High nibbles: H active high (0x7), H blank high (0x1)
    // V active = 1080 (0x438), V blank = 45 (0x02D)
    edid[0x3B] = 0x38;
    edid[0x3C] = 0x2D;
    edid[0x3D] = 0x40; // V active high (0x4), V blank high (0x0)

    // Detailed Descriptor 2 (Monitor Name string) at 0x48
    edid[0x48] = 0x00;
    edid[0x49] = 0x00;
    edid[0x4A] = 0x00;
    edid[0x4B] = 0xFC; // Monitor name tag
    edid[0x4C] = 0x00;
    for (size_t i = 0; i < monitor_name.size() && i < 13; ++i) {
        edid[0x4D + i] = static_cast<uint8_t>(monitor_name[i]);
    }
    if (monitor_name.size() < 13) {
        edid[0x4D + monitor_name.size()] = 0x0A; // Terminated with newline
    }

    fix_edid_checksum(edid.data());
    return edid;
}

} // namespace

int main() {
    std::printf("[test_edid] Starting EDID parser verification...\n");

    // 1. Manufacturer code decoding
    // "DEL": D=4, E=5, L=12 -> (4<<10) | (5<<5) | 12 = 4096 + 160 + 12 = 4268 = 0x10AC
    std::string del_mfg = brodisplays::decode_edid_manufacturer_id(0x10AC);
    CHECK_EQ(del_mfg, "DEL");

    // "SAM": S=19, A=1, M=13 -> (19<<10) | (1<<5) | 13 = 19456 + 32 + 13 = 19501 = 0x4C2D
    std::string sam_mfg = brodisplays::decode_edid_manufacturer_id(0x4C2D);
    CHECK_EQ(sam_mfg, "SAM");

    // 2. Full block parsing
    auto del_edid = create_synthetic_edid("DEL", 0xD001, "Dell U2720Q", 60, 34);
    brodisplays::EdidInfo info;
    bool parsed = brodisplays::parse_edid(del_edid.data(), del_edid.size(), info);
    CHECK(parsed);
    CHECK_EQ(info.manufacturer_id, "DEL");
    CHECK_EQ(info.product_code, 0xD001);
    CHECK_EQ(info.serial_number, 0x12345678u);
    CHECK_EQ(info.monitor_name, "Dell U2720Q");
    CHECK_EQ(info.width_cm, 60u);
    CHECK_EQ(info.height_cm, 34u);

    // 3. Mode extraction
    auto modes = brodisplays::extract_edid_modes(del_edid.data(), del_edid.size());
    CHECK(!modes.empty());
    bool found_1080p = false;
    for (const auto& m : modes) {
        if (m.width == 1920 && m.height == 1080) {
            found_1080p = true;
            CHECK(m.refresh_rate >= 59.0 && m.refresh_rate <= 61.0);
        }
    }
    CHECK(found_1080p);

    // 4. Corrupt header test
    auto corrupt_edid = del_edid;
    corrupt_edid[0] = 0x01; // Corrupt magic byte
    brodisplays::EdidInfo corrupt_info;
    bool corrupt_res = brodisplays::parse_edid(corrupt_edid.data(), corrupt_edid.size(), corrupt_info);
    CHECK(!corrupt_res);

    // 5. Invalid checksum test
    auto bad_csum_edid = del_edid;
    bad_csum_edid[127] ^= 0xFF; // Invert checksum byte
    bool bad_csum_res = brodisplays::parse_edid(bad_csum_edid.data(), bad_csum_edid.size(), corrupt_info);
    CHECK(!bad_csum_res);

    // 6. Too short buffer
    bool short_res = brodisplays::parse_edid(del_edid.data(), 64, corrupt_info);
    CHECK(!short_res);

    return bstest::finish("test_edid");
}
