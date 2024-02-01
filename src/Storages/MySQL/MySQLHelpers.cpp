#include "MySQLHelpers.h"

#if USE_MYSQL
#include <mysqlxx/PoolWithFailover.h>
#include <Storages/MySQL/MySQLSettings.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

mysqlxx::PoolWithFailover createMySQLPoolWithFailover(const StorageMySQL::Configuration & configuration, const MySQLSettings & mysql_settings)
{
    return createMySQLPoolWithFailover(
        configuration.database, configuration.addresses,
        configuration.username, configuration.password,
        configuration.ssl_ca, configuration.ssl_cert,
        configuration.ssl_key, mysql_settings);
}

mysqlxx::PoolWithFailover createMySQLPoolWithFailover(
    const std::string & database,
    const StorageMySQL::Configuration::Addresses & addresses,
    const std::string & username,
    const std::string & password,
    const std::string & ssl_ca,
    const std::string & ssl_cert,
    const std::string & ssl_key,
    const MySQLSettings & mysql_settings)
{
    if (!mysql_settings.connection_pool_size)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Connection pool cannot have zero size");

    return mysqlxx::PoolWithFailover(
        database, addresses, username, password, ssl_ca, ssl_cert, ssl_key,
        MYSQLXX_POOL_WITH_FAILOVER_DEFAULT_START_CONNECTIONS,
        static_cast<unsigned>(mysql_settings.connection_pool_size),
        mysql_settings.connection_max_tries,
        mysql_settings.connection_wait_timeout,
        mysql_settings.connect_timeout,
        mysql_settings.read_write_timeout);
}

}

#endif
