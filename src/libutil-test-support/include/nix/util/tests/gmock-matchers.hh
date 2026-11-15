#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/terminal.hh"
#include <gmock/gmock.h>

namespace nix::testing {

namespace internal {

/**
 * GMock matcher that matches substring while stripping off all ANSI escapes.
 * Useful for checking exceptions messages in unit tests.
 */
class HasSubstrIgnoreANSIMatcher
{
public:
    explicit HasSubstrIgnoreANSIMatcher(std::string substring)
        : substring(std::move(substring))
    {
    }

    bool MatchAndExplain(const char * s, ::testing::MatchResultListener * listener) const
    {
        return s != nullptr && MatchAndExplain(std::string(s), listener);
    }

    template<typename MatcheeStringType>
    bool MatchAndExplain(const MatcheeStringType & s, [[maybe_unused]] ::testing::MatchResultListener * listener) const
    {
        return filterANSIEscapes(s, /*filterAll=*/true).find(substring) != substring.npos;
    }

    void DescribeTo(::std::ostream * os) const
    {
        *os << "has substring " << substring;
    }

    void DescribeNegationTo(::std::ostream * os) const
    {
        *os << "has no substring " << substring;
    }

private:
    std::string substring;
};

} // namespace internal

inline ::testing::PolymorphicMatcher<internal::HasSubstrIgnoreANSIMatcher>
HasSubstrIgnoreANSIMatcher(const std::string & substring)
{
    return ::testing::MakePolymorphicMatcher(internal::HasSubstrIgnoreANSIMatcher(substring));
}

/**
 * Matches a callable that throws `SysError` whose `errNo` equals `expected`.
 *
 * Example:
 *
 *     EXPECT_THAT([&]{ openFile("nope"); }, ThrowsSysError(ENOENT));
 */
inline auto ThrowsSysError(int expected)
{
    return ::testing::Throws<SysError>(::testing::Field(&SysError::errNo, expected));
}

/**
 * Matches a (pointer to a) `SourceAccessor` that has a regular file at
 * `path` with contents `expected`.
 */
MATCHER_P2(HasContents, path, expected, "")
{
    auto stat = arg->maybeLstat(path);
    if (!stat) {
        *result_listener << arg->showPath(path) << " does not exist";
        return false;
    }
    if (stat->type != SourceAccessor::tRegular) {
        *result_listener << arg->showPath(path) << " is not a regular file";
        return false;
    }
    auto actual = arg->readFile(path);
    if (actual != expected) {
        *result_listener << arg->showPath(path) << " has contents " << ::testing::PrintToString(actual);
        return false;
    }
    return true;
}

} // namespace nix::testing
