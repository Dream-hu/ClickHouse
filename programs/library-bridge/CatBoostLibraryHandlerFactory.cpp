#include "CatBoostLibraryHandlerFactory.h"

#include <Common/logger_useful.h>

namespace DB
{

CatBoostLibraryHandlerFactory & CatBoostLibraryHandlerFactory::instance()
{
    static CatBoostLibraryHandlerFactory instance;
    return instance;
}

CatBoostLibraryHandlerPtr CatBoostLibraryHandlerFactory::get(const String & model_path)
{
    std::lock_guard lock(mutex);

    if (auto handler = library_handlers.find(model_path); handler != library_handlers.end())
        return handler->second;
    return nullptr;
}

void CatBoostLibraryHandlerFactory::create(const std::string & library_path, const std::string & model_path)
{
    std::lock_guard lock(mutex);

    if (library_handlers.contains(model_path))
    {
        LOG_WARNING(&Poco::Logger::get("CatBoostLibraryHandlerFactory"), "CatBoost library handler for model path {} already exists.", model_path);
        return;
    }

    library_handlers.emplace(std::make_pair(model_path, std::make_shared<CatBoostLibraryHandler>(library_path, model_path)));
}

}
