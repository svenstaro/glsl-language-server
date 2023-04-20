#include "CLI/CLI.hpp"

#include "fmt/format.h"
#include "fmt/ostream.h"

#include "nlohmann/json.hpp"

#include "mongoose.h"

#include "ResourceLimits.h"
#include "ShaderLang.h"
#include "Initialize.h"

#include <cstdint>
#include <experimental/filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <vector>
#include <map>

#include "messagebuffer.hpp"
#include "workspace.hpp"
#include "utils.hpp"

using json = nlohmann::json;
namespace fs = std::experimental::filesystem;

struct AppState {
    Workspace workspace;
    bool verbose;
    bool use_logfile;
    std::ofstream logfile_stream;
};

std::ofstream* tmp_log;

std::string make_response(const json& response)
{
    json content = response;
    content["jsonrpc"] = "2.0";

    std::string header;
    header.append("Content-Length: " + std::to_string(content.dump(4).size()) + "\r\n");
    header.append("Content-Type: application/vscode-jsonrpc;charset=utf-8\r\n");
    header.append("\r\n");
    return header + content.dump(4);
}

EShLanguage find_language(const std::string& name)
{
    auto ext = fs::path(name).extension();
    if (ext == ".vert" || ext == ".vs")
        return EShLangVertex;
    else if (ext == ".tesc")
        return EShLangTessControl;
    else if (ext == ".tese")
        return EShLangTessEvaluation;
    else if (ext == ".geom" || ext == ".gs")
        return EShLangGeometry;
    else if (ext == ".frag" || ext == ".fs")
        return EShLangFragment;
    else if (ext == ".comp")
        return EShLangCompute;
    throw std::invalid_argument("Unknown file extension!");
}

json get_diagnostics(std::string uri, std::string content,
        AppState& appstate)
{
    FILE fp_old = *stdout;
    *stdout = *fopen("/dev/null","w");
    auto document = uri;
    auto shader_cstring = content.c_str();
    auto lang = find_language(document);
    glslang::TShader shader(lang);
    shader.setStrings(&shader_cstring, 1);
    TBuiltInResource Resources = *GetDefaultResources();
    EShMessages messages = EShMsgCascadingErrors;
    shader.parse(&Resources, 110, false, messages);
    std::string debug_log = shader.getInfoLog();
    *stdout = fp_old;

    if (appstate.use_logfile && appstate.verbose) {
        fmt::print(appstate.logfile_stream, "Diagnostics raw output: {}\n" , debug_log);
    }

    std::regex re("(.*): 0:(\\d*): (.*)");
    std::smatch matches;
    auto error_lines = split_string(debug_log, "\n");
    auto content_lines = split_string(content, "\n");

    json diagnostics;
    for (auto error_line : error_lines) {
        std::regex_search(error_line, matches, re);
        if (matches.size() == 4) {
            json diagnostic;
            std::string severity = matches[1];
            int severity_no = -1;
            if (severity == "ERROR") {
                severity_no = 1;
            } else if (severity == "WARNING") {
                severity_no = 2;
            }
            if (severity_no == -1) {
                if (appstate.use_logfile) {
                    fmt::print(appstate.logfile_stream, "Error: Unknown severity '{}'\n", severity);
                }
            }
            std::string message = trim(matches[3], " ");

            // -1 because lines are 0-indexed as per LSP specification.
            int line_no = std::stoi(matches[2]) - 1;
            std::string source_line = content_lines[line_no];

            int start_char = -1;
            int end_char = -1;

            // If this is an undeclared identifier, we can find the exact
            // position of the broken identifier.
            std::smatch message_matches;
            std::regex re("'(.*)' : (.*)");
            std::regex_search(message, message_matches, re);
            if (message_matches.size() == 3) {
                std::string identifier = message_matches[1];
                int identifier_length = message_matches[1].length();
                auto source_pos = source_line.find(identifier);
                start_char = source_pos;
                end_char = source_pos + identifier_length - 1;
            } else {
                // If we can't find a precise position, we'll just use the whole line.
                start_char = 0;
                end_char = source_line.length();
            }

            json range{
                {"start", {
                    { "line", line_no },
                    { "character", start_char },
                }},
                { "end", {
                    { "line", line_no },
                    { "character", end_char },
                }},
            };
            diagnostic["range"] = range;
            diagnostic["severity"] = severity_no;
            diagnostic["source"] = "glslang";
            diagnostic["message"] = message;
            diagnostics.push_back(diagnostic);
        }
    }
    if (appstate.use_logfile && appstate.verbose && !diagnostics.empty()) {
        fmt::print(appstate.logfile_stream, "Sending diagnostics: {}\n" , diagnostics);
    }
    appstate.logfile_stream.flush();
    return diagnostics;
}

/// Returns the byte offset for the given character on the given line.
// FIXME: use UTF-16 offsets
// https://fasterthanli.me/articles/the-bottom-emoji-breaks-rust-analyzer
int find_position_offset(const char* text, int line, int character) {
    int offset = 0;
    while (line > 0) {
        while (text[offset] && text[offset] != '\n') offset += 1;
        offset += text[offset] == '\n';
        line -= 1;
    }

    while (character > 0 && text[offset] && text[offset] != '\n') {
        offset += 1;
        character -= 1;
    }

    return offset;
}

/// Returns `true` if the character may start an identifier.
bool is_identifier_start_char(char c) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_';
}

/// Returns `true` if the character may be part of an identifier.
bool is_identifier_char(char c) {
    return is_identifier_start_char(c) || ('0' <= c && c <= '9');
}

/// Returns the offset in `text` where the last word started.
int get_last_word_start(const char* text, int offset) {
    int start = offset;
    while (start > 0 && is_identifier_char(text[start - 1])) {
        start -= 1;
    }

    // If `text` was `123abc` and `offset` pointed at `b`, start would point at `1`.
    // We want to point to `a`, so advance past any characters that are not a
    // valid start of an identifier.
    while (start < offset && !is_identifier_start_char(text[start])) {
        start += 1;
    }

    return start;
}

int get_word_end(const char* text, int start) {
    int end = start;
    while (text[end] && is_identifier_char(text[end])) end++;
    return end;
}

struct Symbol {
    enum Kind {
        Unknown = 0,
        Function = 3,
        Type = 7,
        Constant = 21,
    };

    Kind kind = Unknown;
    std::string details;
};

typedef std::map<std::string, Symbol> SymbolMap;

void add_builtin_types(SymbolMap& symbols)  {
    symbols.emplace("bool", Symbol{Symbol::Type, "<type>"});
    symbols.emplace("int", Symbol{Symbol::Type, "<type>"});
    symbols.emplace("uint", Symbol{Symbol::Type, "<type>"});
    symbols.emplace("float", Symbol{Symbol::Type, "<type>"});
    symbols.emplace("double", Symbol{Symbol::Type, "<type>"});

    std::string vec_buffer = "TvecX";
    for (int i = 2; i <= 4; i++) {
        vec_buffer[4] = '0' + i;

        // vec2, vec3, vec4, etc.
        symbols.emplace(&vec_buffer[1], Symbol{Symbol::Type, "<type>"});
        // bvecX, ivecX, uvecX, dvecX
        vec_buffer[0] = 'b';
        symbols.emplace(vec_buffer, Symbol{Symbol::Type, "<type>"});
        vec_buffer[0] = 'i';
        symbols.emplace(vec_buffer, Symbol{Symbol::Type, "<type>"});
        vec_buffer[0] = 'u';
        symbols.emplace(vec_buffer, Symbol{Symbol::Type, "<type>"});
        vec_buffer[0] = 'd';
        symbols.emplace(vec_buffer, Symbol{Symbol::Type, "<type>"});
    }

    std::string mat_buffer = "dmatXxX";
    for (int col = 2; col <= 4; col++) {
        mat_buffer[4] = '0' + col;

        for (int row = 2; row <= 4; row++) {
            mat_buffer[6] = '0' + row;
            symbols.emplace(&mat_buffer[1], Symbol{Symbol::Type, "<type>"});
            symbols.emplace(mat_buffer, Symbol{Symbol::Type, "<type>"});
        }

        mat_buffer[5] = 0;
        symbols.emplace(&mat_buffer[0], Symbol{Symbol::Type, "<type>"});
        symbols.emplace(&mat_buffer[1], Symbol{Symbol::Type, "<type>"});
        mat_buffer[5] = 'x';
    }
    
    const char* image_kinds[] = {
        "1D",
        "2D",
        "3D",
        "Cube",
        "2DRect",
        "1DArray",
        "2DArray",
        "CubeArray",
        "Buffer",
        "2DMS",
        "2DMSArray",
    };

    int image_count = sizeof(image_kinds) / sizeof(image_kinds[0]);
    for (int i = 0; i < image_count; i++) {
        std::string buffer = "gimage";
        buffer += image_kinds[i];

        symbols.emplace(&buffer[1], Symbol{Symbol::Type, "<type>"});
        buffer[0] = 'i';
        symbols.emplace(buffer, Symbol{Symbol::Type, "<type>"});
        buffer[0] = 'u';
        symbols.emplace(buffer, Symbol{Symbol::Type, "<type>"});
    }

    for (int i = 0; i < image_count; i++) {
        std::string buffer = "gsampler";
        buffer += image_kinds[i];

        symbols.emplace(&buffer[1], Symbol{Symbol::Type, "<type>"});
        buffer[0] = 'i';
        symbols.emplace(buffer, Symbol{Symbol::Type, "<type>"});
        buffer[0] = 'u';
        symbols.emplace(buffer, Symbol{Symbol::Type, "<type>"});
    }

    const char* shadow_samplers[] = {
        "sampler1DShadow",
        "sampler2DShadow",
        "samplerCubeShadow",
        "sampler2DRectShadow",
        "sampler1DArrayShadow",
        "sampler2DArrayShadow",
        "samplerCubeArrayShadow",
    };
    int shadow_sampler_count = sizeof(shadow_samplers) / sizeof(shadow_samplers[0]);
    for (int i = 0; i < shadow_sampler_count; i++) {
        symbols.emplace(shadow_samplers[i], Symbol{Symbol::Type, "<type>"});
    }
}

struct Word {
    const char* start = nullptr;
    const char* end = nullptr;

    bool is_equal(const char* text) const {
        const char* s = start;
        while (s != end && *s == *text) {
            s++;
            text++;
        }
        return s == end && *text == 0;
    }
};

bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

/// Extracts all symbols from the given string, and inserts them into the symbol map.
///
/// The current implementation is very naive and may not handle certain cases that well.
void extract_symbols(const char* text, SymbolMap& symbols) {
    std::vector<Word> words;
    int arguments = 0;
    bool had_arguments = false;
    Word inside_block{};

    const char* p = text;
    while (*p) {
        if (is_identifier_start_char(*p)) {
            const char* start = p;
            while (is_identifier_char(*p)) p++;
            if (*p == '[') {
                while (*p && *p != ']') p++;
            }
            Word ident{start, p};

            // don't confuse `layout(...)` for a function.
            if (ident.is_equal("layout")) {
                while (is_whitespace(*p)) p++;
                if (*p == '(') {
                    while (*p && *p != ')') p++;
                }
                continue;
            }

            words.push_back(ident);
            continue;
        } 

        // don't confuse numeric literals as identifiers
        if ('0' <= *p && *p <= '9') {
            p++;
            while (is_identifier_char(*p)) p++;
            continue;
        } 

        if (*p == '{') {
            // TODO: handle function bodies

            if (words.size() >= 2 && arguments == 0) {
                Word kind = words[words.size() - 2];
                if (kind.is_equal("in") 
                        || kind.is_equal("out") 
                        || kind.is_equal("uniform") 
                        || kind.is_equal("buffer")) {
                    inside_block = words[words.size() - 1];
                    words.clear();
                    p++;
                    continue;
                }
            }

            // skip struct fields and function bodies (their contents are not global)
            while (*p && *p != '}') p++;
            continue;
        } 

        if (*p == '}' && inside_block.start) {
            words.push_back(inside_block);
            inside_block = Word{};
        }

        if (*p == '(') {
            had_arguments = true;
            p++;
            const char* start = nullptr;
            const char* end = nullptr;
            while (*p) {
                if (is_whitespace(*p)) {
                    p++;
                    continue;
                }

                if (*p == ')' || *p == ',') {
                    if (start) {
                        words.push_back({start, p});
                        arguments++;
                    }

                    if (*p == ')') break;

                    p++;
                    start = nullptr;
                    end = nullptr;
                    continue;
                }

                if (!start) start = p;
                end = p;

                p++;
            }
        } 

        if (*p == ';' || *p == ')') {
            // end of declaration
            int name_index = (int)words.size() - arguments - 1;
            int type_index = name_index - 1;

            if (name_index >= 0) {
                Word name_word = words[name_index];
                Word type_word = type_index >= 0 ? words[type_index] : Word{};

                std::string name(name_word.start, name_word.end);
                std::string type(type_word.start, type_word.end);

                if (!type.empty()) {
                    symbols.emplace(type, Symbol{Symbol::Type, "<type>"});
                }

                for (int i = 0; i < arguments; i++) {
                    if (i == 0) {
                        type += " (";
                    } else {
                        type += ", ";
                    }


                    Word arg = words[name_index + 1 + i];
                    const char* t = arg.start;
                    while (t != arg.end) {
                        if (is_whitespace(*t)) {
                            // only emit a single space
                            type.push_back(' ');
                            while (t != arg.end && is_whitespace(*t)) t++;
                        } else {
                            type.push_back(*t);
                            t++;
                        }
                    }

                    if (i == arguments - 1) {
                        type += ")";
                    }
                }

                Symbol::Kind kind = *p == ')' ? Symbol::Function : Symbol::Constant;
                symbols.emplace(name, Symbol{kind, type});
            }

            words.clear();
            arguments = 0;
            had_arguments = false;
        }

        p++;
    }
}

SymbolMap get_symbols(const std::string& uri, AppState& appstate){
    auto language = find_language(uri);

    // use the highest known version so that we get as many symbols as possible
    int version = 460;
    // same thing here: use compatibility profile for more symbols
    EProfile profile = ECompatibilityProfile;
    // we don't care about SPIR-V generation
    glslang::SpvVersion spv_version{};

    glslang::TPoolAllocator pool{};
    glslang::SetThreadPoolAllocator(&pool);
    pool.push();

    const TBuiltInResource& resources = *GetDefaultResources();
    glslang::TBuiltIns builtins{};
    builtins.initialize(version, profile, spv_version);
    builtins.initialize(resources, version, profile, spv_version, language);

    // TODO: cache builtin symbols between runs.
    SymbolMap symbols;
    extract_symbols(builtins.getCommonString().c_str(), symbols);
    extract_symbols(builtins.getStageString(language).c_str(), symbols);
    extract_symbols(appstate.workspace.documents()[uri].c_str(), symbols);

    add_builtin_types(symbols);

    glslang::GetThreadPoolAllocator().pop();
    glslang::SetThreadPoolAllocator(nullptr);

    return symbols;
}

void find_completions(const SymbolMap& symbols, const std::string& prefix, std::vector<json>& out) {
    for (auto& entry : symbols) {
        auto& name = entry.first;
        if (name.size() < prefix.size()) continue;
        if (strncmp(name.c_str(), prefix.c_str(), prefix.size()) != 0) continue;

        auto& symbol = entry.second;
        out.push_back(json {
            { "label", name },
            { "kind", symbol.kind == Symbol::Unknown ? json(nullptr) : json(symbol.kind) },
            { "detail", symbol.details },
        });
    }
}

json get_completions(const std::string &uri, int line, int character, AppState& appstate)
{
    const std::string& document = appstate.workspace.documents()[uri];
    int offset = find_position_offset(document.c_str(), line, character);
    int word_start = get_last_word_start(document.c_str(), offset);
    int length = offset - word_start;

    if (length <= 0) {
        // no word under the cursor.
        return nullptr;
    }

    auto name = document.substr(word_start, length);

    std::vector<json> matches;
    auto symbols = get_symbols(uri, appstate);
    find_completions(symbols, name, matches);

    return matches;
}

json get_hover_info(const std::string& uri, int line, int character, AppState& appstate) {
    const std::string& document = appstate.workspace.documents()[uri];
    int offset = find_position_offset(document.c_str(), line, character);
    int word_start = get_last_word_start(document.c_str(), offset);
    int word_end = get_word_end(document.c_str(), word_start);
    int length = word_end - word_start;

    if (length <= 0) {
        // no word under the cursor.
        return nullptr;
    }

    std::string word = document.substr(word_start, length);

    auto symbols = get_symbols(uri, appstate);
    auto symbol = symbols.find(word);
    if (symbol == symbols.end()) return nullptr;

    return json {
        { "contents", { 
            { "language", "glsl" }, 
            { "value", symbol->second.details } 
        } }
    };
}

std::optional<std::string> handle_message(const MessageBuffer& message_buffer, AppState& appstate)
{
    json body = message_buffer.body();

    if (body["method"] == "initialized") {
        return std::nullopt;
    }

    if (body["method"] == "initialize") {
        appstate.workspace.set_initialized(true);

        json text_document_sync{
            { "openClose", true },
            { "change", 1 }, // Full sync
            { "willSave", false },
            { "willSaveWaitUntil", false },
            { "save", { { "includeText", false } } },
        };

        json completion_provider{
            { "resolveProvider", false },
            { "triggerCharacters", {} },
        };
        json signature_help_provider{
            { "triggerCharacters", "" }
        };
        json code_lens_provider{
            { "resolveProvider", false }
        };
        json document_on_type_formatting_provider{
            { "firstTriggerCharacter", "" },
            { "moreTriggerCharacter", "" },
        };
        json document_link_provider{
            { "resolveProvider", false }
        };
        json execute_command_provider{
            { "commands", {} }
        };
        json result{
            {
                "capabilities",
                {
                { "textDocumentSync", text_document_sync },
                { "hoverProvider", true },
                { "completionProvider", completion_provider },
                { "signatureHelpProvider", signature_help_provider },
                { "definitionProvider", false },
                { "referencesProvider", false },
                { "documentHighlightProvider", false },
                { "documentSymbolProvider", false },
                { "workspaceSymbolProvider", false },
                { "codeActionProvider", false },
                { "codeLensProvider", code_lens_provider },
                { "documentFormattingProvider", false },
                { "documentRangeFormattingProvider", false },
                { "documentOnTypeFormattingProvider", document_on_type_formatting_provider },
                { "renameProvider", false },
                { "documentLinkProvider", document_link_provider },
                { "executeCommandProvider", execute_command_provider },
                { "experimental", {} }, }
            }
        };

        json result_body{
            { "id", body["id"] },
            { "result", result }
        };
        return make_response(result_body);
    } else if (body["method"] == "textDocument/didOpen") {
        auto uri = body["params"]["textDocument"]["uri"];
        auto text = body["params"]["textDocument"]["text"];
        appstate.workspace.add_document(uri, text);

        json diagnostics = get_diagnostics(uri, text, appstate);
        if (diagnostics.empty()) {
            diagnostics = json::array();
        }
        json result_body{
            { "method", "textDocument/publishDiagnostics" },
            { "params", {
                            { "uri", uri },
                            { "diagnostics", diagnostics },
                        } }
        };
        return make_response(result_body);
    } else if (body["method"] == "textDocument/didChange") {
        auto uri = body["params"]["textDocument"]["uri"];
        auto change = body["params"]["contentChanges"][0]["text"];
        appstate.workspace.change_document(uri, change);

        std::string document = appstate.workspace.documents()[uri];
        json diagnostics = get_diagnostics(uri, document, appstate);
        if (diagnostics.empty()) {
            diagnostics = json::array();
        }
        json result_body{
            { "method", "textDocument/publishDiagnostics" },
            { "params", {
                            { "uri", uri },
                            { "diagnostics", diagnostics },
                        } }
        };
        return make_response(result_body);
    } else if (body["method"] == "textDocument/completion") {
        auto uri = body["params"]["textDocument"]["uri"];
        auto position = body["params"]["position"];
        int line = position["line"];
        int character = position["character"];

        json completions = get_completions(uri, line, character, appstate);

        json result_body{
            { "id", body["id"] },
            { "result", completions }
        };
        return make_response(result_body);
    } else if (body["method"] == "textDocument/hover") {
        auto uri = body["params"]["textDocument"]["uri"];
        auto position = body["params"]["position"];
        int line = position["line"];
        int character = position["character"];

        json hover = get_hover_info(uri, line, character, appstate);

        json result_body{
            { "id", body["id"] },
            { "result", hover }
        };
        return make_response(result_body);
    }


    // If the workspace has not yet been initialized but the client sends a
    // message that doesn't have method "initialize" then we'll return an error
    // as per LSP spec.
    if (body["method"] != "initialize" && !appstate.workspace.is_initialized()) {
        json error{
            { "code", -32002 },
            { "message", "Server not yet initialized." },
        };
        json result_body{
            { "error", error }
        };
        return make_response(result_body);
    }

    // If we don't know the method requested, we end up here.
    if (body.count("method") == 1) {
        // Requests have an ID field, but notifications do not.
        bool is_notification = body.find("id") == body.end();
        if (is_notification) {
            // We don't have to respond to notifications. So don't error on
            // notifications we don't recognize.
            // https://microsoft.github.io/language-server-protocol/specifications/specification-3-15/#notificationMessage
            return std::nullopt;
        }

        json error{
            { "code", -32601 },
            { "message", fmt::format("Method '{}' not supported.", body["method"].get<std::string>()) },
        };
        json result_body{
            { "id", body["id"] },
            { "error", error },
        };
        return make_response(result_body);
    }

    // If we couldn't parse anything we end up here.
    json error{
        { "code", -32700 },
        { "message", "Couldn't parse message." },
    };
    json result_body{
        { "error", error }
    };
    return make_response(result_body);
}

void ev_handler(struct mg_connection* c, int ev, void* p) {
    AppState& appstate = *static_cast<AppState*>(c->mgr->user_data);

    if (ev == MG_EV_HTTP_REQUEST) {
        struct http_message* hm = (struct http_message*)p;

        std::string content = hm->message.p;

        MessageBuffer message_buffer;
        message_buffer.handle_string(content);

        if (message_buffer.message_completed()) {
            json body = message_buffer.body();
            if (appstate.use_logfile) {
                fmt::print(appstate.logfile_stream, ">>> Received message of type '{}'\n", body["method"].get<std::string>());
                if (appstate.verbose) {
                    fmt::print(appstate.logfile_stream, "Headers:\n");
                    for (auto elem : message_buffer.headers()) {
                        auto pretty_header = fmt::format("{}: {}\n", elem.first, elem.second);
                        appstate.logfile_stream << pretty_header;
                    }
                    fmt::print(appstate.logfile_stream, "Body: \n{}\n\n", body.dump(4));
                    fmt::print(appstate.logfile_stream, "Raw: \n{}\n\n", message_buffer.raw());
                }
            }

            auto message = handle_message(message_buffer, appstate);
            if (message.has_value()) {
                std::string response = message.value();
                mg_send_head(c, 200, response.length(), "Content-Type: text/plain");
                mg_printf(c, "%.*s", static_cast<int>(response.length()), response.c_str());
                if (appstate.use_logfile && appstate.verbose) {
                    fmt::print(appstate.logfile_stream, "<<< Sending message: \n{}\n\n", message.value());
                }
            }
            appstate.logfile_stream.flush();
            message_buffer.clear();
        }
    }
}

int main(int argc, char* argv[])
{
    CLI::App app{ "GLSL Language Server" };

    bool use_stdin = false;
    bool verbose = false;
    uint16_t port = 61313;
    std::string logfile;
    std::string symbols_path;

    auto stdin_option = app.add_flag("--stdin", use_stdin, "Don't launch an HTTP server and instead accept input on stdin");
    app.add_flag("-v,--verbose", verbose, "Enable verbose logging");
    app.add_option("-l,--log", logfile, "Log file");
    app.add_option("-p,--port", port, "Port", true)->excludes(stdin_option);
    app.add_option("--debug-symbols", symbols_path, "Print the builtin symbols");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    AppState appstate;
    appstate.verbose = verbose;
    appstate.use_logfile = !logfile.empty();
    if (appstate.use_logfile) {
        appstate.logfile_stream.open(logfile);
        tmp_log = &appstate.logfile_stream;
    }

    glslang::InitializeProcess();

    if (!symbols_path.empty()) {
        auto symbols = get_symbols(symbols_path, appstate);
        for (auto& entry : symbols) {
            fmt::print("{} : {} : {}\n", entry.first, entry.second.kind, entry.second.details);
        }
    } else if (!use_stdin) {
        struct mg_mgr mgr;
        struct mg_connection* nc;
        struct mg_bind_opts bind_opts;
        std::memset(&bind_opts, 0, sizeof(bind_opts));
        bind_opts.user_data = &appstate;

        mg_mgr_init(&mgr, NULL);
        fmt::print("Starting web server on port {}\n", port);
        nc = mg_bind_opt(&mgr, std::to_string(port).c_str(), ev_handler, bind_opts);
        if (nc == NULL) {
            return 1;
        }

        // Set up HTTP server parameters
        mg_set_protocol_http_websocket(nc);

        while (true) {
            mg_mgr_poll(&mgr, 1000);
        }
        mg_mgr_free(&mgr);
    } else {
        char c;
        MessageBuffer message_buffer;
        while (std::cin.get(c)) {
            message_buffer.handle_char(c);

            if (message_buffer.message_completed()) {
                json body = message_buffer.body();
                if (appstate.use_logfile) {
                    fmt::print(appstate.logfile_stream, ">>> Received message of type '{}'\n", body["method"].get<std::string>());
                    if (appstate.verbose) {
                        fmt::print(appstate.logfile_stream, "Headers:\n");
                        for (auto elem : message_buffer.headers()) {
                            auto pretty_header = fmt::format("{}: {}\n", elem.first, elem.second);
                            appstate.logfile_stream << pretty_header;
                        }
                        fmt::print(appstate.logfile_stream, "Body: \n{}\n\n", body.dump(4));
                        fmt::print(appstate.logfile_stream, "Raw: \n{}\n\n", message_buffer.raw());
                    }
                }

                auto message = handle_message(message_buffer, appstate);
                if (message.has_value()) {
                    fmt::print("{}", message.value());
                    std::cout << std::flush;

                    if (appstate.use_logfile && appstate.verbose) {
                        fmt::print(appstate.logfile_stream, "<<< Sending message: \n{}\n\n", message.value());
                    }
                }
                appstate.logfile_stream.flush();
                message_buffer.clear();
            }
        }
    }

    if (appstate.use_logfile) {
        appstate.logfile_stream.close();
    }

    glslang::FinalizeProcess();

    return 0;
}
