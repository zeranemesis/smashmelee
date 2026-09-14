#include "harness.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace meleeboard::test {

namespace {

struct Case {
    std::string suite;
    std::string name;
    CaseBody body = nullptr;
};

struct CaseAbort {};

// Function-local storage keeps registration working regardless of the order in
// which translation units run their static initializers.
std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

std::vector<std::string>& current_failures()
{
    static std::vector<std::string> failures;
    return failures;
}

} // namespace

int register_case(const char* suite, const char* name, CaseBody body)
{
    registry().push_back({ suite, name, body });
    return 0;
}

void record_failure(const char* file, int line, const std::string& message)
{
    current_failures().push_back(std::string(file) + ":" +
                                 std::to_string(line) + ": " + message);
}

void abort_case() { throw CaseAbort{}; }

using SetupHook = void (*)();

SetupHook& environment_setup()
{
    static SetupHook setup = nullptr;
    return setup;
}

void set_environment_setup(SetupHook setup) { environment_setup() = setup; }

int run_all(int argc, char** argv)
{
    if (environment_setup() != nullptr) {
        environment_setup()();
    }

    const char* filter = argc > 1 ? argv[1] : nullptr;

    std::vector<Case>& cases = registry();
    std::stable_sort(cases.begin(), cases.end(), [](const Case& a, const Case& b) {
        return a.suite < b.suite;
    });

    uint32_t passed = 0;
    uint32_t failed = 0;
    uint32_t skipped = 0;
    for (const Case& test_case : cases) {
        const std::string full_name = test_case.suite + "." + test_case.name;
        if (filter != nullptr && full_name.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }
        current_failures().clear();
        try {
            test_case.body();
        } catch (const CaseAbort&) {
            // The case already recorded why it stopped.
        } catch (const std::exception& error) {
            record_failure("<exception>", 0, error.what());
        } catch (...) {
            record_failure("<exception>", 0, "unknown exception");
        }
        if (current_failures().empty()) {
            ++passed;
            std::printf("[  PASS  ] %s\n", full_name.c_str());
        } else {
            ++failed;
            std::printf("[  FAIL  ] %s\n", full_name.c_str());
            for (const std::string& failure : current_failures()) {
                std::printf("           %s\n", failure.c_str());
            }
        }
        std::fflush(stdout);
    }

    std::printf("\n%u passed, %u failed", passed, failed);
    if (skipped != 0) {
        std::printf(", %u filtered out", skipped);
    }
    std::printf("\n");
    return failed == 0 ? 0 : 1;
}

} // namespace meleeboard::test

int main(int argc, char** argv) { return meleeboard::test::run_all(argc, argv); }
