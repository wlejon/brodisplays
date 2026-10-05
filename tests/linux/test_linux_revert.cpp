#include "common/revert_scenario.h"

int main() {
    std::printf("[test_linux_revert] Linux test-then-revert verification\n");
    return bdtest::run_revert_scenario("test_linux_revert");
}
