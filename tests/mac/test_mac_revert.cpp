#include "common/revert_scenario.h"

int main() {
    std::printf("[test_mac_revert] macOS test-then-revert verification\n");
    return bdtest::run_revert_scenario("test_mac_revert");
}
