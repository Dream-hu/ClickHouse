#pragma once

#include <Common/logger_useful.h>
#include <Interpreters/Context.h>
#include <Server/HTTP/HTTPRequestHandler.h>


namespace DB
{

/// Handler for requests to Library Dictionary Source, returns response in RowBinary format.
/// When a library dictionary source is created, it sends 'extDict_libNew' request to library bridge (which is started on first
/// request to it, if it was not yet started). On this request a new sharedLibrayHandler is added to a
/// sharedLibraryHandlerFactory by a dictionary uuid. With 'extDict_libNew' request come: library_path, library_settings,
/// names of dictionary attributes, sample block to parse block of null values, block of null values. Everything is
/// passed in binary format and is urlencoded. When dictionary is cloned, a new handler is created.
/// Each handler is unique to dictionary.
class ExternalDictionaryLibraryBridgeRequestHandler : public HTTPRequestHandler, WithContext
{
public:
    ExternalDictionaryLibraryBridgeRequestHandler(size_t keep_alive_timeout_, ContextPtr context_);

    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response) override;

private:
    static constexpr inline auto FORMAT = "RowBinary";

    const size_t keep_alive_timeout;
    Poco::Logger * log;
};


// Handler for checking if the external dictionary library is loaded (used for handshake)
class ExternalDictionaryLibraryBridgeExistsHandler : public HTTPRequestHandler, WithContext
{
public:
    ExternalDictionaryLibraryBridgeExistsHandler(size_t keep_alive_timeout_, ContextPtr context_);

    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response) override;

private:
    const size_t keep_alive_timeout;
    Poco::Logger * log;
};


/// Handler for requests to catboost library. The protocol is expected as follows: (1) Send "catboost_libNew" request to load
/// libcatboost.so into the bridge, (2) send "catboost_GetTreeCount" and catboost_Evaluate" to do the model evaluation.
class CatBoostLibraryBridgeRequestHandler : public HTTPRequestHandler, WithContext
{
public:
    CatBoostLibraryBridgeRequestHandler(size_t keep_alive_timeout_, ContextPtr context_);

    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response) override;

private:
    const size_t keep_alive_timeout;
    Poco::Logger * log;
};


// Handler for pinging the library-bridge for catboost access (used for handshake)
class CatBoostLibraryBridgeExistsHandler : public HTTPRequestHandler, WithContext
{
public:
    CatBoostLibraryBridgeExistsHandler(size_t keep_alive_timeout_, ContextPtr context_);

    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response) override;

private:
    const size_t keep_alive_timeout;
    Poco::Logger * log;
};

}
