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
#include <string>

#include "messagebuffer.hpp"

using json = nlohmann::json;
namespace fs = std::experimental::filesystem;

const char* s_http_port = "8000";
struct mg_serve_http_opts s_http_server_opts;

struct Workspace {
    bool m_initialized = false;
};

std::string make_response(const json& result, const json& error)
{
    json content{
        { "jsonrpc", "2.0" },
        { "id", std::experimental::randint(0, 1000000000) },
    };
    if (!result.empty()) {
        content["result"] = result;
    } else if (!error.empty()) {
        content["error"] = error;
    }

    std::string header;
    header.append("Content-Length: " + std::to_string(content.dump().size()) + "\r\n");
    header.append("Content-Type: application/vscode-jsonrpc;charset=utf-8\r\n");
    header.append("\r\n");
    return header + content.dump();
}

std::string handle_message(const MessageBuffer& message_buffer, Workspace& workspace,
    const std::string& logfile, std::ofstream& logfile_stream,
    bool verbose = false)
{
    json body = message_buffer.body();
    if (verbose) {
        // fmt::print(logfile_stream, "Received message of type '{}'\n", body["method"].get<std::string>());
        // fmt::print(logfile_stream, "Headers:\n");
        for (auto elem : message_buffer.headers()) {
            auto pretty_header = fmt::format("{}: {}\n", elem.first, elem.second);
            fmt::print(logfile_stream, "    {}\n", pretty_header);
            if (!logfile.empty()) {
                logfile_stream << pretty_header << std::endl;
            }
        }
        fmt::print(logfile_stream, "Body: \n{}\n", body.dump(4));
    }
    if (!logfile.empty()) {
        logfile_stream << message_buffer.body() << std::endl;
    }

    // Parse initialize method.
    if (body["method"] == "initialize") {
        workspace.m_initialized = true;

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
            {"triggerCharacters", ""}
        };
        json code_lens_provider{
            {"resolveProvider", false}
        };
        json document_on_type_formatting_provider{
            { "firstTriggerCharacter", "" },
            { "moreTriggerCharacter", "" },
        };
        json document_link_provider{
            {"resolveProvider", false}
        };
        json execute_command_provider{
            {"commands", {}}
        };
        json result{
            { "capabilities",
                {}
                    // { "textDocumentSync", text_document_sync },
                    // { "hoverProvider", false },
                    // { "completionProvider", completion_provider },
                    // { "signatureHelpProvider", signature_help_provider },
                    // { "definitionProvider", false },
                    // { "referencesProvider", false },
                    // { "documentHighlightProvider", false },
                    // { "documentSymbolProvider", false },
                    // { "workspaceSymbolProvider", false },
                    // { "codeActionProvider", false },
                    // { "codeLensProvider", code_lens_provider },
                    // { "documentFormattingProvider", false },
                    // { "documentRangeFormattingProvider", false },
                    // { "documentOnTypeFormattingProvider", document_on_type_formatting_provider },
                    // { "renameProvider", false },
                    // { "documentLinkProvider", document_link_provider },
                    // { "executeCommandProvider", execute_command_provider },
                    // { "experimental", {} }, }
            }
        };
        return make_response(result, {});
    }

    // If the workspace has not yet been initialized but the client sends a
    // message that doesn't have method "initialize" then we'll return an error
    // as per LSP spec.
    if (body["method"] != "initialize" && workspace.m_initialized) {
        json error{
            { "code", -32002 },
            { "message", "Server not yet initialized." },
        };
        return make_response({}, error);
    }

    // If we don't know the method requested, we end up here.
    if (body.count("method") == 1) {
        json error{
            { "code", -32601 },
            { "message", fmt::format("Method '{}' not supported.", body["method"].get<std::string>()) },
        };
        return make_response({}, error);
    }

    // If we couldn't parse anything we end up here.
    json error{
        { "code", -32700 },
        { "message", "Couldn't parse message." },
    };
    return make_response({}, error);
}

void ev_handler(struct mg_connection* c, int ev, void* p)
{
    if (ev == MG_EV_HTTP_REQUEST) {
        struct http_message* hm = (struct http_message*)p;

        std::string content = hm->message.p;
        std::string response = make_response({}, {});
        mg_send_head(c, 200, response.length(), "Content-Type: text/plain");
        mg_printf(c, "%.*s", static_cast<int>(response.length()), response.c_str());
    }
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

    std::ofstream logfile_stream;
    if (!logfile.empty()) {
        logfile_stream.open(logfile);
    }

    Workspace workspace;

    if (!use_stdin) {
        struct mg_mgr mgr;
        struct mg_connection* nc;

        glslang::InitializeProcess();
        auto document = "lol.vert";
        std::ifstream shader_file(document);
        std::string shader_string;
        shader_string.assign(
            (std::istreambuf_iterator<char>(shader_file)),
            (std::istreambuf_iterator<char>()));
        auto shader_cstring = shader_string.c_str();
        auto lang = find_language(document);
        glslang::TShader shader(lang);
        shader.setStrings(&shader_cstring, 1);
        TBuiltInResource Resources = glslang::DefaultTBuiltInResource;
        EShMessages messages = EShMsgDefault;
        shader.parse(&Resources, 110, false, messages);
        // std::cout << shader.getInfoLog() << std::endl;
        std::string debug_log = shader.getInfoLog();
        // fmt::print(debug_log);
        glslang::FinalizeProcess();

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
                auto message = handle_message(message_buffer, workspace, logfile, logfile_stream, verbose);
                fmt::print("{}", message);
                std::cout << std::flush;

                if (!logfile.empty()) {
                    fmt::print(logfile_stream, "{}\n", message);
                }
            }
        }
    }

    if (!logfile.empty()) {
        logfile_stream.close();
    }

    return 0;
}
