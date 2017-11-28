#include <string>
#include <vector>

std::vector<std::string> split_string(const std::string& string_to_split, const std::string& pattern);

std::string trim_right(const std::string& s, const std::string& delimiters);

std::string trim_left(const std::string& s, const std::string& delimiters);

std::string trim(const std::string& s, const std::string& delimiters);
