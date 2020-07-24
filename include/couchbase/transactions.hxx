#pragma once

#include <functional>

#include <couchbase/client/cluster.hxx>

#include <couchbase/transactions/attempt_context.hxx>
#include <couchbase/transactions/logging.hxx>
#include <couchbase/transactions/transaction_config.hxx>
#include <couchbase/transactions/transactions_cleanup.hxx>
#include <couchbase/transactions/transaction_result.hxx>

namespace couchbase
{
namespace transactions
{
    typedef std::function<void(attempt_context&)> logic;

    /**
     * @mainpage
     *
     * @ref examples/game_server.cxx - Shows how the transaction could be integrated into application
     *
     * @example examples/game_server.cxx
     * Shows how the transaction could be integrated into application
     */
    class transactions
    {
      public:
        transactions(cluster& cluster, const transaction_config& config)
          : cluster_(cluster)
          , config_(config)
          , cleanup_(cluster_, config_)
        {
        }

        transaction_result run(const logic& logic)
        {
            transaction_context overall;
            attempt_context ctx(this, overall, config_);
            spdlog::info("starting attempt {}/{}/{}", overall.num_attempts(), overall.transaction_id(), ctx.attempt_id());
            logic(ctx);
            if (!ctx.is_done()) {
                ctx.commit();
            }
            return transaction_result{overall.transaction_id(),
                                      overall.atr_id(),
                                      overall.atr_collection(),
                                      overall.attempts(),
                                      overall.current_attempt().state == attempt_state::COMPLETED};

        }

        void close()
        {
        }

        [[nodiscard]] transactions_cleanup& cleanup()
        {
            return cleanup_;
        }

      private:
        couchbase::cluster& cluster_;
        transaction_config config_;
        transactions_cleanup cleanup_;
    };
} // namespace transactions
} // namespace couchbase