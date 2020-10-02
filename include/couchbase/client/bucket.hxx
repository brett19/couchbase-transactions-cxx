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

#include <memory>
#include <string>
struct lcb_st;

namespace couchbase
{
class collection;
class cluster;
/**
 * Couchbase bucket.
 *
 * Exposes bucket-level operations and collections accessors.
 */
class bucket : public std::enable_shared_from_this<bucket>
{
    friend class collection;
    friend class cluster;

  private:
    lcb_st* lcb_;
    const std::string name_;
    bucket(lcb_st* instance, const std::string& name);

  public:
    std::shared_ptr<class collection> default_collection();
    std::shared_ptr<class collection> collection(const std::string& name);
    const std::string name() const { return name_; };
    void close();
    ~bucket();
    bool operator==(const bucket& b) const { return lcb_ == b.lcb_; }
};
}// namespace couchbase
