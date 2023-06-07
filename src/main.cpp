#include "CLI/CLI.hpp"

#include "fmt/format.h"
#include "fmt/ostream.h"

#include "nlohmann/json.hpp"

#include "mongoose.h"

#include "ResourceLimits.h"
#include "ShaderLang.h"
#include "Initialize.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <vector>
#include <map>

#include "messagebuffer.hpp"
#include "workspace.hpp"
#include "utils.hpp"
#include "symbols.hpp"
#include "includer.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

/// By default we target the most recent graphics APIs to be maximally permissive.
struct TargetVersions {
    // The target API (eg, Vulkan, OpenGL).
    glslang::EShClient client_api = glslang::EShClientVulkan;
    glslang::EShTargetClientVersion client_api_version = glslang::EShTargetVulkan_1_3;

    // The target SPIR-V version
    glslang::EShTargetLanguageVersion spv_version = glslang::EShTargetSpv_1_6;
};

struct AppState {
    Workspace workspace;
    bool verbose;
    bool use_logfile;
    std::ofstream logfile_stream;
    TargetVersions target;
};

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
    if (ext == ".glsl")
        ext = fs::path(name).replace_extension().extension(); //replaces current extension with nothing and finds new file extension
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
    auto lang = find_language(document);

    glslang::TShader shader(lang);

    auto target = appstate.target;
    shader.setEnvClient(target.client_api, target.client_api_version);
    shader.setEnvTarget(glslang::EShTargetSpv, target.spv_version);

    auto shader_cstring = content.c_str();
    auto shader_name = document.c_str();
    shader.setStringsWithLengthsAndNames(&shader_cstring, nullptr, &shader_name, 1);

    FileIncluder includer{&appstate.workspace};

    TBuiltInResource Resources = *GetDefaultResources();
    EShMessages messages =
      (EShMessages)(EShMsgCascadingErrors | EShMsgVulkanRules);
    shader.parse(&Resources, 110, false, messages, includer);
    std::string debug_log = shader.getInfoLog();
    *stdout = fp_old;

    if (appstate.use_logfile && appstate.verbose) {
        fmt::print(appstate.logfile_stream, "Diagnostics raw output: {}\n" , debug_log);
    }

    std::regex re("([A-Z]*): (.*):(\\d*): (.*)");
    std::smatch matches;
    auto error_lines = split_string(debug_log, "\n");
    auto content_lines = split_string(content, "\n");

    json diagnostics;
    for (auto error_line : error_lines) {
        std::regex_search(error_line, matches, re);
        if (matches.size() == 5) {
            std::string file = matches[2];
            if (file != document) continue; // message is for another file

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

            std::string message = trim(matches[4], " ");

            // -1 because lines are 0-indexed as per LSP specification.
            int line_no = std::stoi(matches[3]) - 1;
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
        fmt::print(appstate.logfile_stream, "Sending diagnostics: {}\n" , diagnostics.dump(4));
    }
    appstate.logfile_stream.flush();
    return diagnostics;
}

SymbolMap get_symbols(const std::string& uri, AppState& appstate){
    auto language = find_language(uri);

    // use the highest known version so that we get as many symbols as possible
    int version = 460;
    // same thing here: use compatibility profile for more symbols
    EProfile profile = ECompatibilityProfile;

    glslang::SpvVersion spv_version{};
    spv_version.spv = appstate.target.spv_version;
    spv_version.vulkanRelaxed = true; // be maximally permissive, allowing certain OpenGL in Vulkan

    glslang::TPoolAllocator pool{};
    glslang::SetThreadPoolAllocator(&pool);
    pool.push();

    const TBuiltInResource& resources = *GetDefaultResources();
    glslang::TBuiltIns builtins{};
    builtins.initialize(version, profile, spv_version);
    builtins.initialize(resources, version, profile, spv_version, language);

    // TODO: cache builtin symbols between runs.
    SymbolMap symbols;
    add_builtin_types(symbols);
    extract_symbols(builtins.getCommonString().c_str(), symbols);
    extract_symbols(builtins.getStageString(language).c_str(), symbols);

    extract_symbols(appstate.workspace.documents()[uri].c_str(), symbols, uri.c_str());

    glslang::GetThreadPoolAllocator().pop();
    glslang::SetThreadPoolAllocator(nullptr);

    return symbols;
}

void find_completions(const SymbolMap& symbols, const std::string& prefix, std::vector<json>& out) {
    for (auto& entry : symbols) {
        auto& name = entry.first;
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

std::optional<std::string> get_word_under_cursor(
        const std::string& uri, 
        int line, int character, 
        AppState& appstate) 
{
    const std::string& document = appstate.workspace.documents()[uri];
    int offset = find_position_offset(document.c_str(), line, character);
    int word_start = get_last_word_start(document.c_str(), offset);
    int word_end = get_word_end(document.c_str(), word_start);
    int length = word_end - word_start;

    if (length <= 0) {
        // no word under the cursor.
        return std::nullopt;
    }

    return document.substr(word_start, length);
}

json get_hover_info(const std::string& uri, int line, int character, AppState& appstate) {
    auto word = get_word_under_cursor(uri, line, character, appstate);
    if (!word) return nullptr;

    auto symbols = get_symbols(uri, appstate);
    auto symbol = symbols.find(*word);
    if (symbol == symbols.end()) return nullptr;

    return json {
        { "contents", { 
            { "language", "glsl" }, 
            { "value", symbol->second.details } 
        } }
    };
}

json get_definition(const std::string& uri, int line, int character, AppState& appstate) {
    auto word = get_word_under_cursor(uri, line, character, appstate);
    if (!word) return nullptr;

    auto symbols = get_symbols(uri, appstate);
    auto symbol_iter = symbols.find(*word);
    if (symbol_iter == symbols.end()) return nullptr;
    auto symbol = symbol_iter->second;
    if (symbol.location.uri == nullptr) return nullptr;

    const std::string& text = appstate.workspace.documents()[symbol.location.uri];
    auto position = find_source_location(text.c_str(), symbol.location.offset);
    int length = word->size();

    json start {
        { "line", position.line },
        { "character", position.character },
    };
    json end {
        { "line", position.line },
        { "character", position.character + length },
    };
    return json {
        { "uri", symbol.location.uri },
        { "range", { { "start", start }, { "end", end } } },
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
            { "triggerCharacters", json::array() },
        };
        json signature_help_provider{
            { "triggerCharacters", json::array() }
        };
        json code_lens_provider{
            { "resolveProvider", false }
        };
        json document_on_type_formatting_provider{
            { "firstTriggerCharacter", "" },
            { "moreTriggerCharacter", json::array() },
        };
        json document_link_provider{
            { "resolveProvider", false }
        };
        json execute_command_provider{
            { "commands", json::array() }
        };
        json result{
            {
                "capabilities",
                {
                { "textDocumentSync", text_document_sync },
                { "hoverProvider", true },
                { "completionProvider", completion_provider },
                { "signatureHelpProvider", signature_help_provider },
                { "definitionProvider", true },
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
    } else if (body["method"] == "textDocument/definition") {
        auto uri = body["params"]["textDocument"]["uri"];
        auto position = body["params"]["position"];
        int line = position["line"];
        int character = position["character"];

        json result = get_definition(uri, line, character, appstate);

        json result_body{
            { "id", body["id"] },
            { "result", result }
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

    std::string client_api = "vulkan1.3";
    std::string spirv_version = "spv1.6";

    std::string symbols_path;
    std::string diagnostic_path;

    auto stdin_option = app.add_flag("--stdin", use_stdin, "Don't launch an HTTP server and instead accept input on stdin");
    app.add_flag("-v,--verbose", verbose, "Enable verbose logging");
    app.add_option("-l,--log", logfile, "Log file");
    app.add_option("--debug-symbols", symbols_path, "Print the list of symbols for the given file");
    app.add_option("--debug-diagnostic", diagnostic_path, "Debug diagnostic output for the given file");
    app.add_option("-p,--port", port, "Port")->excludes(stdin_option);
    app.add_option("--target-env", client_api,
            "Target client environment.\n"
            "    [vulkan vulkan1.0 vulkan1.1 vulkan1.2 vulkan1.3 opengl opengl4.5]");
    app.add_option("--target-spv", spirv_version,
            "The SPIR-V version to target.\n"
            "Defaults to the highest possible for the target environment.\n"
            "    [spv1.0 spv1.1 spv1.2 spv1.3 spv1.4 spv1.5 spv1.6]");

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
    }

    if (!client_api.empty()) {
        if (client_api == "vulkan1.3" || client_api == "vulkan") {
            appstate.target.client_api = glslang::EShClientVulkan;
            appstate.target.client_api_version = glslang::EShTargetVulkan_1_3;
            appstate.target.spv_version = glslang::EShTargetSpv_1_6;
        } else if (client_api == "vulkan1.2") {
            appstate.target.client_api = glslang::EShClientVulkan;
            appstate.target.client_api_version = glslang::EShTargetVulkan_1_2;
            appstate.target.spv_version = glslang::EShTargetSpv_1_5;
        } else if (client_api == "vulkan1.1") {
            appstate.target.client_api = glslang::EShClientVulkan;
            appstate.target.client_api_version = glslang::EShTargetVulkan_1_1;
            appstate.target.spv_version = glslang::EShTargetSpv_1_3;
        } else if (client_api == "vulkan1.0") {
            appstate.target.client_api = glslang::EShClientVulkan;
            appstate.target.client_api_version = glslang::EShTargetVulkan_1_0;
            appstate.target.spv_version = glslang::EShTargetSpv_1_1;
        } else if (client_api == "opengl4.5" || client_api == "opengl") {
            appstate.target.client_api = glslang::EShClientOpenGL;
            appstate.target.client_api_version = glslang::EShTargetOpenGL_450;
            appstate.target.spv_version = glslang::EShTargetSpv_1_3;
        } else {
            fmt::print("unknown client api: {}\n", client_api);
            return 1;
        }
    }

    if (!spirv_version.empty()) {
        if (spirv_version == "spv1.6") {
            appstate.target.spv_version = glslang::EShTargetSpv_1_6;
        } else if (spirv_version == "spv1.5") {
            appstate.target.spv_version = glslang::EShTargetSpv_1_5;
        } else if (spirv_version == "spv1.4") {
            appstate.target.spv_version = glslang::EShTargetSpv_1_4;
        } else if (spirv_version == "spv1.3") {
            appstate.target.spv_version = glslang::EShTargetSpv_1_3;
        } else if (spirv_version == "spv1.2") {
            appstate.target.spv_version = glslang::EShTargetSpv_1_2;
        } else if (spirv_version == "spv1.1") {
            appstate.target.spv_version = glslang::EShTargetSpv_1_1;
        } else if (spirv_version == "spv1.0") {
            appstate.target.spv_version = glslang::EShTargetSpv_1_0;
        } else {
            fmt::print("unknown SPIR-V version: {}\n", spirv_version);
            return 1;
        }
    }

    glslang::InitializeProcess();

    if (!symbols_path.empty()) {
        std::string contents = *read_file_to_string(symbols_path.c_str());
        std::string uri = make_path_uri(symbols_path);
        appstate.workspace.add_document(uri, contents);
        auto symbols = get_symbols(uri, appstate);
        for (auto& entry : symbols) {
            const auto& name = entry.first;
            const auto& symbol = entry.second;

            if (symbol.location.uri) {
                const auto& contents = appstate.workspace.documents()[symbol.location.uri];
                auto position = find_source_location(contents.c_str(), symbol.location.offset);
                fmt::print("{} : {}:{} : {}\n", name, position.line, position.character, symbol.details);
            } else {
                fmt::print("{} : @{} : {}\n", name, symbol.location.offset, symbol.details);
            }
        }
    } else if (!diagnostic_path.empty()) {
        std::string contents = *read_file_to_string(diagnostic_path.c_str());
        std::string uri = make_path_uri(diagnostic_path);
        appstate.workspace.add_document(uri, contents);
        auto diagnostics = get_diagnostics(uri, contents, appstate);
        fmt::print("diagnostics: {}\n", diagnostics.dump(4));
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
