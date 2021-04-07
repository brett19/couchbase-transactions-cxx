#include "logging.hxx"

namespace couchbase
{
namespace transactions
{
    // Repeated code as we don't want spdlog in public interface
    spdlog::level::level_enum cb_to_spdlog_level(log_level level)
    {
        switch (level) {
            case log_level::TRACE:
                return spdlog::level::trace;
            case log_level::DEBUG:
                return spdlog::level::debug;
            case log_level::INFO:
                return spdlog::level::info;
            case log_level::WARN:
                return spdlog::level::warn;
            case log_level::ERROR:
                return spdlog::level::err;
            case log_level::CRITICAL:
                return spdlog::level::critical;
            default:
                return spdlog::level::off;
        }
    }

    void set_transactions_log_level(log_level level)
    {
        spdlog::level::level_enum lvl = cb_to_spdlog_level(level);
        txn_log->set_level(lvl);
        attempt_cleanup_log->set_level(lvl);
        lost_attempts_cleanup_log->set_level(lvl);
    }

} // namespace transactions
} // namespace couchbase
