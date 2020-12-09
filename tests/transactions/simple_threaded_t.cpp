#include <thread>

#include "../client/client_env.h"
#include <couchbase/client/cluster.hxx>
#include <couchbase/client/collection.hxx>
#include <couchbase/transactions.hxx>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

using namespace couchbase;

static nlohmann::json content = nlohmann::json::parse("{\"some number\": 0}");

TEST(ThreadedTransactions, CanGetReplace)
{
    auto cluster = ClientTestEnvironment::get_cluster();
    transactions::transaction_config cfg;
    transactions::transactions txn(*cluster, cfg);
    std::vector<std::thread> threads;

    // upsert initial doc
    auto coll = cluster->bucket("default")->default_collection();
    auto id = ClientTestEnvironment::get_uuid();
    ASSERT_TRUE(coll->upsert(id, content).is_success());

    // More threads than we have bucket instances
    int num_threads = 2 * coll->get_bucket()->max_instances();
    int num_iterations = 10;

    // use counter to be sure all txns were successful
    std::atomic<uint64_t> counter{ 0 };
    std::atomic<uint64_t> expired{ 0 };
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&]() {
            try {
                for (int j = 0; j < num_iterations; j++) {
                    txn.run([&](transactions::attempt_context& ctx) {
                        auto doc = ctx.get(coll, id);
                        auto content = doc.content<nlohmann::json>();
                        content["another one"] = ++counter;
                        ctx.replace(coll, doc, content);
                    });
                }
            } catch (const transactions::transaction_expired& e) {
                ++expired;
            }
        });
    }
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    auto final_content = coll->get(id).value->get<nlohmann::json>();
    ASSERT_EQ(final_content["another one"].get<uint64_t>(), counter.load());
    // we increment counter each time through the lambda - chances are high there
    // will be retries, so this will be at least threads*iterations, probably more.
    ASSERT_GE(counter.load(), num_threads * num_iterations);
}

TEST(ThreadedTransactions, CanInsertThenGetRemove)
{
    auto cluster = ClientTestEnvironment::get_cluster();
    transactions::transaction_config cfg;
    transactions::transactions txn(*cluster, cfg);
    std::vector<std::thread> threads;
    auto coll = cluster->bucket("default")->default_collection();

    // More threads than we have bucket instances
    int num_threads = 2 * coll->get_bucket()->max_instances();
    int num_iterations = 10;

    // use counter to be sure all txns were successful
    std::atomic<uint64_t> counter{ 0 };
    std::atomic<uint64_t> expired{ 0 };
    std::string id_prefix = ClientTestEnvironment::get_uuid();

    // threadsafe if only stuff on stack
    std::function<std::string(int)> get_id = [id_prefix](int i) -> std::string {
        std::stringstream stream;
        stream << id_prefix << "_" << i;
        return stream.str();
    };

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&, i]() {
            auto id = get_id(i);
            for (int j = 0; j < num_iterations; j++) {
                try {
                    coll->insert(id, content);
                    txn.run([&](transactions::attempt_context& ctx) {
                        auto doc = ctx.get(coll, id);
                        ctx.remove(coll, doc);
                        ++counter;
                    });
                } catch (transactions::transaction_expired& e) {
                    ++expired;
                }
            }
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    // we are not contending for same doc in this one, so counter should be exact.
    ASSERT_EQ(counter.load(), num_threads * num_iterations);
    // TODO: verify those docs are not there.
    for (int i = 0; i < num_threads; i++) {
        ASSERT_TRUE(coll->get(get_id(i)).is_not_found());
    }
}