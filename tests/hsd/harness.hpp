#pragma once

// A dependency-free test harness for the native HSD port.  The HSD core
// compiles without Aurora, a GPU, or a disc image, so its behavior can be
// pinned down by fast offline tests instead of a manual run on Windows.

#include <cstdint>
#include <sstream>
#include <string>

namespace meleeboard::test {

using CaseBody = void (*)();

int register_case(const char* suite, const char* name, CaseBody body);
void record_failure(const char* file, int line, const std::string& message);
[[noreturn]] void abort_case();
int run_all(int argc, char** argv);

template <typename T> std::string describe(const T& value)
{
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

inline std::string describe(std::nullptr_t) { return "nullptr"; }
inline std::string describe(bool value) { return value ? "true" : "false"; }

} // namespace meleeboard::test

#define MELEE_TEST_CONCAT_INNER(a, b) a##b
#define MELEE_TEST_CONCAT(a, b) MELEE_TEST_CONCAT_INNER(a, b)

// Defines a test case.  `suite` and `name` are bare identifiers.
#define MELEE_TEST(suite, name)                                                \
    static void MELEE_TEST_CONCAT(suite##_##name, _body)();                    \
    static const int MELEE_TEST_CONCAT(suite##_##name, _registered) =          \
        ::meleeboard::test::register_case(#suite, #name,                       \
                                          &MELEE_TEST_CONCAT(suite##_##name,   \
                                                             _body));          \
    static void MELEE_TEST_CONCAT(suite##_##name, _body)()

// Records a failure and keeps going, so one case can report every mismatch.
#define CHECK(expression)                                                      \
    do {                                                                       \
        if (!(expression)) {                                                   \
            ::meleeboard::test::record_failure(__FILE__, __LINE__,             \
                                               "CHECK(" #expression ")");      \
        }                                                                      \
    } while (false)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        const auto melee_test_actual = (actual);                               \
        const auto melee_test_expected = (expected);                           \
        if (!(melee_test_actual == melee_test_expected)) {                      \
            ::meleeboard::test::record_failure(                                \
                __FILE__, __LINE__,                                            \
                std::string(#actual " == " #expected " (actual ") +            \
                    ::meleeboard::test::describe(melee_test_actual) +          \
                    ", expected " +                                            \
                    ::meleeboard::test::describe(melee_test_expected) + ")");  \
        }                                                                      \
    } while (false)

// Records a failure and stops the case; use it when continuing would crash.
#define REQUIRE(expression)                                                    \
    do {                                                                       \
        if (!(expression)) {                                                   \
            ::meleeboard::test::record_failure(__FILE__, __LINE__,             \
                                               "REQUIRE(" #expression ")");    \
            ::meleeboard::test::abort_case();                                  \
        }                                                                      \
    } while (false)

#define REQUIRE_EQ(actual, expected)                                           \
    do {                                                                       \
        const auto melee_test_actual = (actual);                               \
        const auto melee_test_expected = (expected);                           \
        if (!(melee_test_actual == melee_test_expected)) {                      \
            ::meleeboard::test::record_failure(                                \
                __FILE__, __LINE__,                                            \
                std::string(#actual " == " #expected " (actual ") +            \
                    ::meleeboard::test::describe(melee_test_actual) +          \
                    ", expected " +                                            \
                    ::meleeboard::test::describe(melee_test_expected) + ")");  \
            ::meleeboard::test::abort_case();                                  \
        }                                                                      \
    } while (false)
