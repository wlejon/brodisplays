#include "common/revert_scenario.h"

int main() {
    std::printf("[test_win_revert] Windows test-then-revert verification\n");
    return bdtest::run_revert_scenario("test_win_revert");
}
