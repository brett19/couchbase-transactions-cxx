/*
 *     Copyright 2020 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <couchbase/client/bucket.hxx>

struct lcb_st;

namespace couchbase
{
class bucket;

class cluster
{
  private:
    lcb_st* lcb_;
    std::string cluster_address_;
    std::string user_name_;
    std::string password_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<class bucket>> open_buckets_;

    void connect();

  public:
    explicit cluster(std::string cluster_address, std::string user_name, std::string password);
    cluster(const cluster& cluster);
    ~cluster();

    std::list<std::string> buckets();
    std::shared_ptr<class bucket> bucket(const std::string& name);
    bool operator==(const couchbase::cluster&  other) const;
    // Helpful for debugging/logging
    const std::string cluster_address() const
    {
        return cluster_address_;
    }
    void shutdown();
};
} // namespace couchbase
