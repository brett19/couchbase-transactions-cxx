
#include <couchbase/transactions/transaction_context.hxx>

#include "logging.hxx"
#include "uid_generator.hxx"

namespace couchbase
{
namespace transactions
{
    transaction_context::transaction_context()
      : transaction_id_(uid_generator::next())
      , start_time_client_(std::chrono::system_clock::now())
      , deferred_elapsed_(0)
    {
    }

    void transaction_context::add_attempt()
    {
        transaction_attempt attempt{};
        attempts_.push_back(attempt);
    }

    CB_NODISCARD bool transaction_context::has_expired_client_side(const transaction_config& config)
    {
        const auto& now = std::chrono::system_clock::now();
        auto expired_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time_client_) + deferred_elapsed_;
        auto expired_millis = std::chrono::duration_cast<std::chrono::milliseconds>(expired_nanos);
        bool is_expired = expired_nanos > config.expiration_time();
        if (is_expired) {
            spdlog::info("has expired client side (now={}ns, start={}ns, deferred_elapsed={}ns, expired={}ns ({}ms), config={}ms)",
                         now.time_since_epoch().count(),
                         start_time_client_.time_since_epoch().count(),
                         deferred_elapsed_.count(),
                         expired_nanos.count(),
                         expired_millis.count(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(config.expiration_time()).count());
        }
        return is_expired;
    }

    void transaction_context::retry_delay(const transaction_config& config)
    {
        // when we retry an operation, we typically call that function recursively.  So, we need to
        // limit total number of times we do it.  Later we can be more sophisticated perhaps.
        auto delay = config.expiration_time() / 100; // the 100 is arbitrary
        spdlog::trace("about to sleep for {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(delay).count());
        std::this_thread::sleep_for(delay);
    }

} // namespace transactions
} // namespace couchbase