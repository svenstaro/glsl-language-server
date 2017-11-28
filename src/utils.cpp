#include "utils.hpp"

#include <regex>

std::vector<std::string> split_string(const std::string& string_to_split, const std::string& pattern)
{
    std::vector<std::string> result;

    const std::regex re(pattern);
    std::sregex_token_iterator iter(string_to_split.begin(), string_to_split.end(), re, -1);

    for (std::sregex_token_iterator end; iter != end; ++iter) {
        result.push_back(iter->str());
    }

    return result;
}

std::string trim_right(const std::string& s, const std::string& delimiters = " \f\n\r\t\v")
{
    return s.substr(0, s.find_last_not_of(delimiters) + 1);
}

std::string trim_left(const std::string& s, const std::string& delimiters = " \f\n\r\t\v")
{
    return s.substr(s.find_first_not_of(delimiters));
}

std::string trim(const std::string& s, const std::string& delimiters = " \f\n\r\t\v")
{
    return trim_left(trim_right(s, delimiters), delimiters);
}
