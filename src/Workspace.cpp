#include "LSP/Workspace.hpp"

#include <iostream>
#include <climits>

// <<< RRP
#include <pugixml.hpp>
// RRP >>>

#include "glob/glob.hpp"
#include "Luau/BuiltinDefinitions.h"
#include "LSP/LuauExt.hpp"

void WorkspaceFolder::openTextDocument(const lsp::DocumentUri& uri, const lsp::DidOpenTextDocumentParams& params)
{
    auto normalisedUri = fileResolver.normalisedUriString(uri);

    fileResolver.managedFiles.emplace(
        std::make_pair(normalisedUri, TextDocument(uri, params.textDocument.languageId, params.textDocument.version, params.textDocument.text)));

    // Mark the file as dirty as we don't know what changes were made to it
    auto moduleName = fileResolver.getModuleName(uri);
    frontend.markDirty(moduleName);
}

void WorkspaceFolder::updateTextDocument(
    const lsp::DocumentUri& uri, const lsp::DidChangeTextDocumentParams& params, std::vector<Luau::ModuleName>* markedDirty)
{
    auto normalisedUri = fileResolver.normalisedUriString(uri);

    if (!contains(fileResolver.managedFiles, normalisedUri))
    {
        client->sendLogMessage(lsp::MessageType::Error, "Text Document not loaded locally: " + uri.toString());
        return;
    }
    auto& textDocument = fileResolver.managedFiles.at(normalisedUri);
    textDocument.update(params.contentChanges, params.textDocument.version);

    // Mark the module dirty for the typechecker
    auto moduleName = fileResolver.getModuleName(uri);
    frontend.markDirty(moduleName, markedDirty);
}

void WorkspaceFolder::closeTextDocument(const lsp::DocumentUri& uri)
{
    fileResolver.managedFiles.erase(fileResolver.normalisedUriString(uri));

    // Mark the module as dirty as we no longer track its changes
    auto config = client->getConfiguration(rootUri);
    auto moduleName = fileResolver.getModuleName(uri);
    frontend.markDirty(moduleName);

    // Refresh workspace diagnostics to clear diagnostics on ignored files
    if (!config.diagnostics.workspace || isIgnoredFile(uri.fsPath()))
        clearDiagnosticsForFile(uri);
}

void WorkspaceFolder::clearDiagnosticsForFile(const lsp::DocumentUri& uri)
{
    if (!client->capabilities.textDocument || !client->capabilities.textDocument->diagnostic)
    {
        client->publishDiagnostics(lsp::PublishDiagnosticsParams{uri, std::nullopt, {}});
    }
    else if (client->workspaceDiagnosticsToken)
    {
        lsp::WorkspaceDocumentDiagnosticReport documentReport;
        documentReport.uri = uri;
        documentReport.kind = lsp::DocumentDiagnosticReportKind::Full;
        lsp::WorkspaceDiagnosticReportPartialResult report{{documentReport}};
        client->sendProgress({client->workspaceDiagnosticsToken.value(), report});
    }
    else
    {
        client->refreshWorkspaceDiagnostics();
    }
}

/// Whether the file has been marked as ignored by any of the ignored lists in the configuration
bool WorkspaceFolder::isIgnoredFile(const std::filesystem::path& path, const std::optional<ClientConfiguration>& givenConfig)
{
    // We want to test globs against a relative path to workspace, since thats what makes most sense
    auto relativePath = path.lexically_relative(rootUri.fsPath()).generic_string(); // HACK: we convert to generic string so we get '/' separators

    auto config = givenConfig ? *givenConfig : client->getConfiguration(rootUri);
    std::vector<std::string> patterns = config.ignoreGlobs; // TODO: extend further?
    for (auto& pattern : patterns)
    {
        if (glob::fnmatch_case(relativePath, pattern))
        {
            return true;
        }
    }
    return false;
}

bool WorkspaceFolder::isDefinitionFile(const std::filesystem::path& path, const std::optional<ClientConfiguration>& givenConfig)
{
    auto config = givenConfig ? *givenConfig : client->getConfiguration(rootUri);
    auto canonicalised = std::filesystem::weakly_canonical(path);

    for (auto& file : config.types.definitionFiles)
    {
        if (std::filesystem::weakly_canonical(file) == canonicalised)
        {
            return true;
        }
    }

    return false;
}

void WorkspaceFolder::indexFiles(const ClientConfiguration& config)
{
    if (!config.index.enabled)
        return;

    if (isNullWorkspace())
        return;

    size_t indexCount = 0;

    for (std::filesystem::recursive_directory_iterator next(rootUri.fsPath()), end; next != end; ++next)
    {
        if (indexCount >= config.index.maxFiles)
        {
            client->sendWindowMessage(lsp::MessageType::Warning, "The maximum workspace index limit (" + std::to_string(config.index.maxFiles) +
                                                                     ") has been hit. This may cause some language features to only work partially "
                                                                     "(Find All References, Rename). If necessary, consider increasing the limit");
            break;
        }

        if (next->is_regular_file() && next->path().has_extension() && !isDefinitionFile(next->path(), config) &&
            !isIgnoredFile(next->path(), config))
        {
            auto ext = next->path().extension();
            if (ext == ".lua" || ext == ".luau")
            {
                auto moduleName = fileResolver.getModuleName(Uri::file(next->path()));
                // We use autocomplete because its in strict mode, and this is useful for Find All References
                frontend.check(moduleName, Luau::FrontendOptions{/* retainFullTypeGraphs: */ true, /* forAutocomplete: */ true});
                // TODO: do we need indexing for non-autocomplete?
                // frontend.check(moduleName);
                indexCount += 1;
            }
        }
    }
}

// <<< MTA
void WorkspaceFolder::indexMTAFiles()
{
    if (isNullWorkspace())
        return;

    const auto parseMeta = [&](const std::filesystem::path& path) -> bool
    {
        pugi::xml_document doc;
        pugi::xml_parse_result result = doc.load_file(path.c_str());

        if (!result)
        {
            client->sendWindowMessage(lsp::MessageType::Error,
                "Failed to read MTA meta file " + path.string() + ". XML file parse error");
            return false;
        }

        auto meta = doc.child("meta");
        if (!meta)
        {
            client->sendWindowMessage(lsp::MessageType::Error,
                "Failed to read MTA meta file " + path.string() + ". Cannot find a valid root node");
            return false;
        }

        std::shared_ptr<Luau::MTAMetaDescription> desc = std::make_shared<Luau::MTAMetaDescription>();

        for (pugi::xml_node script = meta.child("script"); script; script = script.next_sibling("script"))
        {
            Luau::MTAScriptType scriptType{ Luau::MTAScriptType::Server };
            if (auto type = script.attribute("type"))
            {
                if (std::strcmp(type.value(), "server") == 0)
                    scriptType = Luau::MTAScriptType::Server;
                else if (std::strcmp(type.value(), "client") == 0)
                    scriptType = Luau::MTAScriptType::Client;
                else if (std::strcmp(type.value(), "shared") == 0)
                    scriptType = Luau::MTAScriptType::Shared;
                else
                    client->sendWindowMessage(lsp::MessageType::Warning,
                        "Invalid script type specified. See " + path.string());
            }

            if (auto src = script.attribute("src"))
            {
                auto scriptPath = path.parent_path() / src.value();
                auto scriptModuleName = Uri::parse(Uri::file(scriptPath).toString()).fsPath().generic_string();
    
                desc->files.push_back({scriptType, scriptModuleName});

                frontend.scriptFiles[scriptModuleName] = std::make_pair(desc, scriptType);
            }
            else
                client->sendWindowMessage(lsp::MessageType::Warning,
                        "Script name is not specified. See " + path.string());
        }

        return true;
    };

    for (std::filesystem::recursive_directory_iterator next(rootUri.fsPath()), end; next != end; ++next)
    {
        if (!next->is_regular_file() || !next->path().has_filename())
            continue;

        const auto filename = next->path().filename();
        if (filename == "meta.xml")
            parseMeta(next->path());
    }

    /*
    
    */
    frontend.clear();
    instanceTypes.clear();

    // Prepare module scope so that we can dynamically reassign the type of "script" to retrieve instance info
    frontend.prepareModuleScope = [this](const Luau::ModuleName& name, const Luau::ScopePtr& scope, bool forAutocomplete)
    {
        auto& resolver = forAutocomplete ? frontend.moduleResolverForAutocomplete : frontend.moduleResolver;
        
// <<< RRP
        // Process includes
        {
            Luau::SourceModule* modulePtr = frontend.getSourceModule(name);
            if (!modulePtr)
                return;       

            if (auto foundTrace = frontend.requireTrace.find(name); foundTrace != frontend.requireTrace.end())
            {
                Luau::RequireTraceResult& traceResult = foundTrace->second;

                for (const auto& [includeName, location, typeName] : traceResult.requireList)
                {
                    if (typeName == "require")
                        continue;

                    auto includeModule = resolver.getModule(includeName);
                    if (includeModule)
                        frontend.copyGlobalsFromModule(includeModule, scope, forAutocomplete); 
                }
            }  
        }
// RRP >>>

        // Process shared scripts from meta
        {
            auto found = frontend.scriptFiles.find(name);
            if (found == frontend.scriptFiles.end())
                return;

            const auto& [meta, type] = found->second;
            if (!meta)
                return;

            for (const auto& entry : meta->files)
            {
                auto includeModule = resolver.getModule(entry.name);
                if (!includeModule)
                    continue;

                if (entry.name != name && IsMTAScriptTypeMatched(type, entry.type))
                    frontend.copyGlobalsFromModule(includeModule, scope, forAutocomplete);
            }
        }
    };
}
// MTA >>>

bool WorkspaceFolder::updateSourceMap()
{
    auto sourcemapPath = rootUri.fsPath() / "sourcemap.json";
    client->sendTrace("Updating sourcemap contents from " + sourcemapPath.generic_string());

    // Read in the sourcemap
    // TODO: we assume a sourcemap.json file in the workspace root
    if (auto sourceMapContents = readFile(sourcemapPath))
    {
        frontend.clear();
        fileResolver.updateSourceMap(sourceMapContents.value());

        // Recreate instance types
        auto config = client->getConfiguration(rootUri);
        instanceTypes.clear();
        // NOTE: expressive types is always enabled for autocomplete, regardless of the setting!
        // We pass the same setting even when we are registering autocomplete globals since
        // the setting impacts what happens to diagnostics (as both calls overwrite frontend.prepareModuleScope)
        types::registerInstanceTypes(frontend, frontend.globals, instanceTypes, fileResolver,
            /* expressiveTypes: */ config.diagnostics.strictDatamodelTypes);
        types::registerInstanceTypes(frontend, frontend.globalsForAutocomplete, instanceTypes, fileResolver,
            /* expressiveTypes: */ config.diagnostics.strictDatamodelTypes);

        return true;
    }
    else
    {
        return false;
    }
}

void WorkspaceFolder::initialize()
{
    Luau::registerBuiltinGlobals(frontend, frontend.globals, /* typeCheckForAutocomplete = */ false);
    Luau::registerBuiltinGlobals(frontend, frontend.globalsForAutocomplete, /* typeCheckForAutocomplete = */ true);

    Luau::attachTag(Luau::getGlobalBinding(frontend.globalsForAutocomplete, "require"), "Require");

    if (client->definitionsFiles.empty())
    {
        client->sendLogMessage(lsp::MessageType::Warning, "No definitions file provided by client");
    }

    for (const auto& definitionsFile : client->definitionsFiles)
    {
        client->sendLogMessage(lsp::MessageType::Info, "Loading definitions file: " + definitionsFile.generic_string());

        auto definitionsContents = readFile(definitionsFile);
        if (!definitionsContents)
        {
            client->sendWindowMessage(lsp::MessageType::Error,
                "Failed to read definitions file " + definitionsFile.generic_string() + ". Extended types will not be provided");
            continue;
        }

        auto result = types::registerDefinitions(frontend, frontend.globals, *definitionsContents, /* typeCheckForAutocomplete = */ false);
        types::registerDefinitions(frontend, frontend.globalsForAutocomplete, *definitionsContents, /* typeCheckForAutocomplete = */ true);

        auto uri = Uri::file(definitionsFile);

        if (result.success)
        {
            // Clear any set diagnostics
            client->publishDiagnostics({uri, std::nullopt, {}});
        }
        else
        {
            client->sendWindowMessage(lsp::MessageType::Error,
                "Failed to read definitions file " + definitionsFile.generic_string() + ". Extended types will not be provided");

            // Display relevant diagnostics
            std::vector<lsp::Diagnostic> diagnostics;
            for (auto& error : result.parseResult.errors)
                diagnostics.emplace_back(createParseErrorDiagnostic(error));

            if (result.module)
                for (auto& error : result.module->errors)
                    diagnostics.emplace_back(createTypeErrorDiagnostic(error, &fileResolver));

            client->publishDiagnostics({uri, std::nullopt, diagnostics});
        }
    }
    Luau::freeze(frontend.globals.globalTypes);
    Luau::freeze(frontend.globalsForAutocomplete.globalTypes);
}

void WorkspaceFolder::setupWithConfiguration(const ClientConfiguration& configuration)
{
    isConfigured = true;
    if (configuration.sourcemap.enabled)
    {
        if (!isNullWorkspace() && !updateSourceMap())
        {
            client->sendWindowMessage(
                lsp::MessageType::Error, "Failed to load sourcemap.json for workspace '" + name + "'. Instance information will not be available");
        }
    }

    if (configuration.index.enabled)
        indexFiles(configuration);

// <<< MTA
    indexMTAFiles(); 
// MTA >>>
}