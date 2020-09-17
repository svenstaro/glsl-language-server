#ifndef MESSAGEBUFFER_H
#define MESSAGEBUFFER_H

#include "nlohmann/json.hpp"

#include <string>
#include <tuple>

using json = nlohmann::json;

class MessageBuffer {
public:
    MessageBuffer();
    virtual ~MessageBuffer();
    void handle_char(char c);
    void handle_string(std::string s);
    const std::map<std::string, std::string>& headers() const;
    const json& body() const;
    const std::string& raw() const;
    bool message_completed();
    void clear();

private:
    std::tuple<std::string, std::string> try_parse_header(std::string &message) const;

    std::string m_raw_message;
    std::map<std::string, std::string> m_headers;
    json m_body;

    // This is set once a sole \r\n is encountered because it denotes that the
    // header is done.
    bool m_is_header_done = false;
};

#endif /* MESSAGEBUFFER_H */
