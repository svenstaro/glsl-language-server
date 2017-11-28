#include "CLI/CLI.hpp"

#include "fmt/format.h"
#include "fmt/ostream.h"

#include "json.hpp"

#include "mongoose.h"

#include "ResourceLimits.h"
#include "ShaderLang.h"

#include <cstdint>
#include <experimental/filesystem>
#include <experimental/random>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "messagebuffer.hpp"
#include "workspace.hpp"
#include "utils.hpp"

using json = nlohmann::json;
namespace fs = std::experimental::filesystem;

const char* s_http_port = "8000";
struct mg_serve_http_opts s_http_server_opts;

std::string make_response(const json& response)
{
    json content = response;
    content["jsonrpc"] = "2.0";
    content["id"] = std::experimental::randint(0, 1000000000);

    std::string header;
    header.append("Content-Length: " + std::to_string(content.dump(4).size()) + "\r\n");
    header.append("Content-Type: application/vscode-jsonrpc;charset=utf-8\r\n");
    header.append("\r\n");
    return header + content.dump(4);
}

EShLanguage find_language(const std::string& name)
{
    auto ext = fs::path(name).extension();
    if (ext == ".vert")
        return EShLangVertex;
    else if (ext == ".tesc")
        return EShLangTessControl;
    else if (ext == ".tese")
        return EShLangTessEvaluation;
    else if (ext == ".geom")
        return EShLangGeometry;
    else if (ext == ".frag")
        return EShLangFragment;
    else if (ext == ".comp")
        return EShLangCompute;
}

json get_diagnostics(std::string uri, std::string content,
        bool use_logfile, std::ostream& logfile_stream)
{
    glslang::InitializeProcess();
    auto document = uri;
    auto shader_cstring = content.c_str();
    auto lang = find_language(document);
    glslang::TShader shader(lang);
    shader.setStrings(&shader_cstring, 1);
    TBuiltInResource Resources = glslang::DefaultTBuiltInResource;
    EShMessages messages = EShMsgCascadingErrors;
    shader.parse(&Resources, 110, false, messages);
    std::string debug_log = shader.getInfoLog();
    glslang::FinalizeProcess();

    if (use_logfile) {
        fmt::print(logfile_stream, "Diagnostics raw output: {}\n" , debug_log);
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
                if (use_logfile) {
                    fmt::print(logfile_stream, "Error: Unknown severity '{}'\n", severity);
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
    if (use_logfile) {
        fmt::print(logfile_stream, "Sending diagnostics: {}\n" , diagnostics);
    }
    logfile_stream.flush();
    return diagnostics;
}

std::optional<std::string> handle_message(const MessageBuffer& message_buffer, Workspace& workspace,
    bool use_logfile, std::ofstream& logfile_stream, bool verbose = false)
{
    json body = message_buffer.body();

    if (body["method"] == "initialize") {
        workspace.set_initialized(true);

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
                { "hoverProvider", false },
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
            { "result", result }
        };
        return make_response(result_body);
    } else if (body["method"] == "textDocument/didOpen") {
        auto uri = body["params"]["textDocument"]["uri"];
        auto text = body["params"]["textDocument"]["text"];
        workspace.add_document(uri, text);

        json diagnostics = get_diagnostics(uri, text, use_logfile, logfile_stream);
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
        workspace.change_document(uri, change);

        std::string document = workspace.documents()[uri];
        json diagnostics = get_diagnostics(uri, document, use_logfile, logfile_stream);
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
    }

    // If the workspace has not yet been initialized but the client sends a
    // message that doesn't have method "initialize" then we'll return an error
    // as per LSP spec.
    if (body["method"] != "initialize" && !workspace.is_initialized()) {
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
        json error{
            { "code", -32601 },
            { "message", fmt::format("Method '{}' not supported.", body["method"].get<std::string>()) },
        };
        json result_body{
            { "error", error }
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

void ev_handler(struct mg_connection* c, int ev, void* p)
{
    if (ev == MG_EV_HTTP_REQUEST) {
        struct http_message* hm = (struct http_message*)p;

        std::string content = hm->message.p;
        std::string response = make_response({});
        mg_send_head(c, 200, response.length(), "Content-Type: text/plain");
        mg_printf(c, "%.*s", static_cast<int>(response.length()), response.c_str());
    }
}

int main(int argc, char* argv[])
{
    CLI::App app{ "GLSL Language Server" };

    bool use_stdin = false;
    bool verbose = false;
    uint16_t port = 61313;
    std::string logfile;
    auto stdin_option = app.add_flag("--stdin", use_stdin, "Don't launch an HTTP server and instead accept input on stdin");
    app.add_flag("-v,--verbose", verbose, "Enable verbose logging");
    app.add_option("-l,--log", logfile, "Log file");
    app.add_option("-p,--port", port, "Port", true)->excludes(stdin_option);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    bool use_logfile = !logfile.empty();
    std::ofstream logfile_stream;
    if (use_logfile) {
        logfile_stream.open(logfile);
    }

    Workspace workspace;

    // TODO: HTTP Stuff probably doesn't work right now.
    if (!use_stdin) {
        struct mg_mgr mgr;
        struct mg_connection* nc;

        mg_mgr_init(&mgr, NULL);
        // printf("Starting web server on port %s\n", s_http_port);
        nc = mg_bind(&mgr, s_http_port, ev_handler);
        if (nc == NULL) {
            // printf("Failed to create listener\n");
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
                if (use_logfile) {
                    fmt::print(logfile_stream, ">>> Received message of type '{}'\n", body["method"].get<std::string>());
                    fmt::print(logfile_stream, "Headers:\n");
                    for (auto elem : message_buffer.headers()) {
                        auto pretty_header = fmt::format("{}: {}\n", elem.first, elem.second);
                        logfile_stream << pretty_header;
                    }
                    fmt::print(logfile_stream, "Body: \n{}\n\n", body.dump(4));
                    fmt::print(logfile_stream, "Raw: \n{}\n\n", message_buffer.raw());
                }

                auto message = handle_message(message_buffer, workspace,
                        use_logfile, logfile_stream);
                if (message.has_value()) {
                    fmt::print("{}", message.value());
                    std::cout << std::flush;

                    if (use_logfile) {
                        fmt::print(logfile_stream, "<<< Sending message: \n{}\n\n", message.value());
                    }
                }
                logfile_stream.flush();
                message_buffer.clear();
            }
        }
    }

    if (use_logfile) {
        logfile_stream.close();
    }

    return 0;
}
