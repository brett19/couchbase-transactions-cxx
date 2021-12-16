/*
 *     Copyright 2021 Couchbase, Inc.
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

#include "attempt_context_impl.hxx"
#include "active_transaction_record.hxx"
#include "atr_ids.hxx"
#include "attempt_context_testing_hooks.hxx"
#include "couchbase/transactions/internal/logging.hxx"
#include "exceptions_internal.hxx"
#include "forward_compat.hxx"
#include "staged_mutation.hxx"
#include "utils.hxx"

#include <couchbase/transactions/attempt_state.hxx>

namespace couchbase::transactions
{
cluster&
attempt_context_impl::cluster_ref()
{
    return overall_.cluster_ref();
}

attempt_context_impl::attempt_context_impl(transaction_context& transaction_ctx)
  : overall_(transaction_ctx)
  , config_(transaction_ctx.config())
  , is_done_(false)
  , staged_mutations_(new staged_mutation_queue())
  , hooks_(config_.attempt_context_hooks())
{
    // put a new transaction_attempt in the context...
    overall_.add_attempt();
    trace("added new attempt, state {}", state());
}

attempt_context_impl::~attempt_context_impl() = default;

// not a member of attempt_context_impl, as forward_compat is internal.
template<typename Handler>
void
attempt_context_impl::check_and_handle_blocking_transactions(const transaction_get_result& doc, forward_compat_stage stage, Handler&& cb)
{
    // The main reason to require doc to be fetched inside the transaction is we can detect this on the client side
    if (doc.links().has_staged_write()) {
        // Check not just writing the same doc twice in the same transaction
        // NOTE: we check the transaction rather than attempt id. This is to handle [RETRY-ERR-AMBIG-REPLACE].
        if (doc.links().staged_transaction_id().value() == transaction_id()) {
            debug("doc {} has been written by this transaction, ok to continue", doc.id());
            return cb(std::nullopt);
        }
        if (doc.links().atr_id() && doc.links().atr_bucket_name() && doc.links().staged_attempt_id()) {
            debug("doc {} in another txn, checking atr...", doc.id());
            auto err = forward_compat::check(stage, doc.links().forward_compat());
            if (err) {
                return cb(err);
            }
            exp_delay delay(std::chrono::milliseconds(50), std::chrono::milliseconds(500), std::chrono::seconds(1));
            return check_atr_entry_for_blocking_document(doc, delay, cb);
        }
        debug("doc {} is in another transaction {}, but doesn't have enough info to check the atr. "
              "probably a bug, proceeding to overwrite",
              doc.id(),
              *doc.links().staged_attempt_id());
    }
    return cb(std::nullopt);
}

transaction_get_result
attempt_context_impl::get(const couchbase::document_id& id)
{
    auto barrier = std::make_shared<std::promise<transaction_get_result>>();
    auto f = barrier->get_future();
    get(id, [barrier](std::optional<transaction_operation_failed> err, std::optional<transaction_get_result> res) {
        if (err) {
            barrier->set_exception(std::make_exception_ptr(*err));
        } else {
            barrier->set_value(*res);
        }
    });
    return f.get();
}
void
attempt_context_impl::get(const couchbase::document_id& id, Callback&& cb)
{
    cache_error_async(std::move(cb), [&]() {
        check_if_done(cb);
        do_get(id, [this, id, cb = std::move(cb)](std::optional<error_class> ec, std::optional<transaction_get_result> res) {
            if (!ec) {
                ec = hooks_.after_get_complete(this, id.key());
            }
            if (ec) {
                switch (*ec) {
                    case FAIL_EXPIRY:
                        return op_completed_with_error(std::move(cb),
                                                       transaction_operation_failed(*ec, "transaction expired during get").expired());
                    case FAIL_DOC_NOT_FOUND:
                        return op_completed_with_error(std::move(cb), transaction_operation_failed(*ec, "document not found"));
                    case FAIL_TRANSIENT:
                        return op_completed_with_error(std::move(cb),
                                                       transaction_operation_failed(*ec, "transient failure in get").retry());
                    case FAIL_HARD:
                        return op_completed_with_error(std::move(cb), transaction_operation_failed(*ec, "fail hard in get").no_rollback());
                    default: {
                        std::string msg("got error while getting doc ");
                        return op_completed_with_error(std::move(cb), transaction_operation_failed(FAIL_OTHER, msg.append(id.key())));
                    }
                }
            } else {
                if (!res) {
                    return op_completed_with_error(std::move(cb), transaction_operation_failed(*ec, "document not found"));
                }
                auto err = forward_compat::check(forward_compat_stage::GETS, res->links().forward_compat());
                if (err) {
                    return op_completed_with_error(std::move(cb), *err);
                }
                return op_completed_with_callback(std::move(cb), res);
            }
        });
    });
}

std::optional<transaction_get_result>
attempt_context_impl::get_optional(const couchbase::document_id& id)
{
    auto barrier = std::make_shared<std::promise<std::optional<transaction_get_result>>>();
    auto f = barrier->get_future();
    get_optional(id, [barrier](std::optional<transaction_operation_failed> err, std::optional<transaction_get_result> res) {
        if (err) {
            return barrier->set_exception(std::make_exception_ptr(*err));
        }
        return barrier->set_value(res);
    });
    return f.get();
}

void
attempt_context_impl::get_optional(const couchbase::document_id& id, Callback&& cb)
{
    cache_error_async(std::move(cb), [&]() {
        check_if_done(cb);
        do_get(id, [this, id, cb = std::move(cb)](std::optional<error_class> ec, std::optional<transaction_get_result> res) {
            if (!ec) {
                ec = hooks_.after_get_complete(this, id.key());
            }
            if (ec) {
                switch (*ec) {
                    case FAIL_EXPIRY:
                        return op_completed_with_error(std::move(cb),
                                                       transaction_operation_failed(*ec, "transaction expired during get").expired());
                    case FAIL_DOC_NOT_FOUND:
                        return op_completed_with_callback(std::move(cb), std::nullopt);
                    case FAIL_TRANSIENT:
                        return op_completed_with_error(std::move(cb),
                                                       transaction_operation_failed(*ec, "transient failure in get").retry());
                    case FAIL_HARD:
                        return op_completed_with_error(std::move(cb), transaction_operation_failed(*ec, "fail hard in get").no_rollback());
                    default: {
                        std::string msg("got error while getting doc ");
                        return op_completed_with_error(std::move(cb), transaction_operation_failed(FAIL_OTHER, msg.append(id.key())));
                    }
                }
            } else {
                if (res) {
                    auto err = forward_compat::check(forward_compat_stage::GETS, res->links().forward_compat());
                    if (err) {
                        return op_completed_with_error(std::move(cb), *err);
                    }
                }
                return op_completed_with_callback(std::move(cb), res);
            }
        });
    });
}

couchbase::operations::mutate_in_request
attempt_context_impl::create_staging_request(const transaction_get_result& document,
                                             const std::string type,
                                             std::optional<std::string> content)
{
    couchbase::operations::mutate_in_request req{ document.id() };
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, TRANSACTION_ID, jsonify(overall_.transaction_id()));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, ATTEMPT_ID, jsonify(id()));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, ATR_ID, jsonify(atr_id()));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, ATR_BUCKET_NAME, jsonify(document.id().bucket()));
    req.specs.add_spec(
      protocol::subdoc_opcode::dict_upsert, true, true, false, ATR_COLL_NAME, jsonify(collection_spec_from_id(atr_id_.value())));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, true, CRC32_OF_STAGING, mutate_in_macro::VALUE_CRC_32C);
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, TYPE, jsonify(type));

    if (document.metadata()) {
        if (document.metadata()->cas()) {
            req.specs.add_spec(
              protocol::subdoc_opcode::dict_upsert, true, true, false, PRE_TXN_CAS, jsonify(document.metadata()->cas().value()));
        }
        if (document.metadata()->revid()) {
            req.specs.add_spec(
              protocol::subdoc_opcode::dict_upsert, true, true, false, PRE_TXN_REVID, jsonify(document.metadata()->revid().value()));
        }
        if (document.metadata()->exptime()) {
            req.specs.add_spec(
              protocol::subdoc_opcode::dict_upsert, true, true, false, PRE_TXN_EXPTIME, jsonify(document.metadata()->exptime().value()));
        }
    }
    if (type != "remove") {
        req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, STAGED_DATA, content.value());
    }
    return wrap_durable_request(req, config_);
}

void
attempt_context_impl::replace_raw(const transaction_get_result& document, const std::string& content, Callback&& cb)
{
    return cache_error_async(std::move(cb), [&]() {
        try {
            trace("replacing {} with {}", document, content);
            check_if_done(cb);
            if (check_expiry_pre_commit(STAGE_REPLACE, document.id().key())) {
                return op_completed_with_error(cb, transaction_operation_failed(FAIL_EXPIRY, "transaction expired").expired());
            }
            check_and_handle_blocking_transactions(
              document,
              forward_compat_stage::WWC_REPLACING,
              [this, document = std::move(document), cb = std::move(cb), content](std::optional<transaction_operation_failed> err) {
                  if (err) {
                      return op_completed_with_error(cb, *err);
                  }
                  select_atr_if_needed_unlocked(
                    document.id(),
                    [this, document = std::move(document), cb = std::move(cb), content](std::optional<transaction_operation_failed> err) {
                        if (err) {
                            return op_completed_with_error(cb, *err);
                        }
                        auto req = create_staging_request(document, "replace", content);
                        req.cas.value = document.cas();
                        req.access_deleted = true;
                        auto error_handler = [this, cb](error_class ec, const std::string& msg) {
                            transaction_operation_failed err(ec, msg);
                            switch (ec) {
                                case FAIL_DOC_NOT_FOUND:
                                case FAIL_DOC_ALREADY_EXISTS:
                                case FAIL_CAS_MISMATCH:
                                case FAIL_TRANSIENT:
                                case FAIL_AMBIGUOUS:
                                    return op_completed_with_error(std::move(cb), err.retry());
                                case FAIL_HARD:
                                    return op_completed_with_error(std::move(cb), err.no_rollback());
                                default:
                                    return op_completed_with_error(std::move(cb), err);
                            }
                        };
                        auto ec = hooks_.before_staged_replace(this, document.id().key());
                        if (ec) {
                            return error_handler(*ec, "bdfore_staged_replace hook raised error");
                        }
                        trace("about to replace doc {} with cas {} in txn {}", document.id(), document.cas(), overall_.transaction_id());
                        overall_.cluster_ref().execute(
                          req,
                          [this, document = std::move(document), content, cb, error_handler = std::move(error_handler)](
                            couchbase::operations::mutate_in_response resp) {
                              auto ec = error_class_from_response(resp);
                              if (!ec) {
                                  auto err = hooks_.after_staged_replace_complete(this, document.id().key());
                                  if (err) {
                                      return error_handler(*err, "after_staged_replace_commit hook returned error");
                                  }
                                  transaction_get_result out = document;
                                  out.cas(resp.cas.value);
                                  trace("replace staged content, result {}", out);
                                  staged_mutation* existing_replace = staged_mutations_->find_replace(document.id());
                                  staged_mutation* existing_insert = staged_mutations_->find_insert(document.id());
                                  if (existing_replace != nullptr) {
                                      trace("document {} was replaced already in txn, replacing again", document.id());
                                      // only thing that we need to change are the content, cas
                                      existing_replace->content(content);
                                      existing_replace->doc().cas(out.cas());
                                  } else if (existing_insert != nullptr) {
                                      trace("document {} replaced after insert in this txn", document.id());
                                      // only thing that we need to change are the content, cas
                                      existing_insert->doc().content(content);
                                      existing_insert->doc().cas(out.cas());
                                  } else {
                                      staged_mutations_->add(staged_mutation(out, content, staged_mutation_type::REPLACE));
                                  }
                                  return op_completed_with_callback(std::move(cb), out);
                              } else {
                                  return error_handler(*ec, resp.ctx.ec.message());
                              }
                          });
                    });
              });
        } catch (const client_error& e) {
            error_class ec = e.ec();
            switch (ec) {
                case FAIL_EXPIRY:
                    expiry_overtime_mode_ = true;
                    throw transaction_operation_failed(ec, e.what()).expired();
                default:
                    throw transaction_operation_failed(ec, e.what());
            }
        }
    });
}

transaction_get_result
attempt_context_impl::replace_raw(const transaction_get_result& document, const std::string& content)
{
    auto barrier = std::make_shared<std::promise<transaction_get_result>>();
    auto f = barrier->get_future();
    replace_raw(document, content, [barrier](std::optional<transaction_operation_failed> err, std::optional<transaction_get_result> res) {
        if (err) {
            return barrier->set_exception(std::make_exception_ptr(*err));
        }
        barrier->set_value(*res);
    });
    return f.get();
}
transaction_get_result
attempt_context_impl::insert_raw(const couchbase::document_id& id, const std::string& content)
{
    auto barrier = std::make_shared<std::promise<transaction_get_result>>();
    auto f = barrier->get_future();
    insert_raw(id, content, [barrier](std::optional<transaction_operation_failed> err, std::optional<transaction_get_result> res) {
        if (err) {
            return barrier->set_exception(std::make_exception_ptr(*err));
        }
        barrier->set_value(*res);
    });
    return f.get();
}
void
attempt_context_impl::insert_raw(const couchbase::document_id& id, const std::string& content, Callback&& cb)
{
    return cache_error_async(std::move(cb), [&]() {
        try {
            check_if_done(cb);
            if (check_for_own_write(id)) {
                return op_completed_with_error(
                  std::move(cb),
                  transaction_operation_failed(FAIL_OTHER, "cannot insert a document that has already been mutated in this transaction"));
            }
            if (check_expiry_pre_commit(STAGE_INSERT, id.key())) {
                return op_completed_with_error(std::move(cb), transaction_operation_failed(FAIL_EXPIRY, "transaction expired").expired());
            }
            select_atr_if_needed_unlocked(id, [this, cb = std::move(cb), id, content](std::optional<transaction_operation_failed> err) {
                if (err) {
                    return op_completed_with_error(cb, *err);
                }
                uint64_t cas = 0;
                exp_delay delay(std::chrono::milliseconds(5), std::chrono::milliseconds(300), config_.expiration_time());
                create_staged_insert(id, content, cas, delay, cb);
            });
        } catch (const std::exception& e) {
            return op_completed_with_error(std::move(cb), transaction_operation_failed(FAIL_OTHER, e.what()));
        }
    });
}

void
attempt_context_impl::select_atr_if_needed_unlocked(const couchbase::document_id& id,
                                                    std::function<void(std::optional<transaction_operation_failed>)>&& cb)
{
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        if (atr_id_) {
            trace("atr exists, moving on");
            return cb(std::nullopt);
        }
        size_t vbucket_id = 0;
        std::optional<const std::string> hook_atr = hooks_.random_atr_id_for_vbucket(this);
        if (hook_atr) {
            atr_id_ = { id.bucket(), "_default", "_default", *hook_atr };
        } else {
            vbucket_id = atr_ids::vbucket_for_key(id.key());
            atr_id_ = { id.bucket(), "_default", "_default", atr_ids::atr_id_for_vbucket(vbucket_id) };
        }
        overall_.atr_collection(collection_spec_from_id(id));
        overall_.atr_id(atr_id_->key());
        state(attempt_state::NOT_STARTED);
        trace("first mutated doc in transaction is \"{}\" on vbucket {}, so using atr \"{}\"", id, vbucket_id, atr_id_.value());
        set_atr_pending_locked(id, std::move(lock), cb);
    } catch (const std::exception& e) {
        error("unexpected error {} during select atr if needed");
    }
}
template<typename Handler, typename Delay>
void
attempt_context_impl::check_atr_entry_for_blocking_document(const transaction_get_result& doc, Delay delay, Handler&& cb)
{
    try {
        delay();
        if (auto ec = hooks_.before_check_atr_entry_for_blocking_doc(this, doc.id().key())) {
            return cb(transaction_operation_failed(FAIL_WRITE_WRITE_CONFLICT, "document is in another transaction").retry());
        }
        couchbase::document_id atr_id(doc.links().atr_bucket_name().value(),
                                      doc.links().atr_scope_name().value(),
                                      doc.links().atr_collection_name().value(),
                                      doc.links().atr_id().value());
        active_transaction_record::get_atr(
          cluster_ref(),
          atr_id,
          [this, delay = std::move(delay), cb = std::move(cb), doc = std::move(doc)](std::error_code err,
                                                                                     std::optional<active_transaction_record> atr) {
              if (!err) {
                  auto entries = atr->entries();
                  auto it = std::find_if(entries.begin(), entries.end(), [&doc](const atr_entry& e) {
                      return e.attempt_id() == doc.links().staged_attempt_id();
                  });
                  if (it != entries.end()) {
                      auto err = forward_compat::check(forward_compat_stage::WWC_READING_ATR, it->forward_compat());
                      if (err) {
                          return cb(err);
                      }
                      if (it->has_expired()) {
                          debug("existing atr entry has expired (age is {}ms), ignoring", it->age_ms());
                          return cb(std::nullopt);
                      }
                      switch (it->state()) {
                          case attempt_state::COMPLETED:
                          case attempt_state::ROLLED_BACK:
                              debug("existing atr entry can be ignored due to state {}", it->state());
                              return cb(std::nullopt);
                          default:
                              debug("existing atr entry found in state {}, retrying", it->state());
                      }
                      return check_atr_entry_for_blocking_document(doc, delay, cb);
                  } else {
                      debug("no blocking atr entry");
                      return cb(std::nullopt);
                  }
              }
              // if we are here, there is still a write-write conflict
              return cb(transaction_operation_failed(FAIL_WRITE_WRITE_CONFLICT, "document is in another transaction").retry());
          });
    } catch (const retry_operation_timeout& t) {
        return cb(transaction_operation_failed(FAIL_WRITE_WRITE_CONFLICT, "document is in another transaction").retry());
    }
}
void
attempt_context_impl::remove(transaction_get_result& document, VoidCallback&& cb)
{
    return cache_error_async(std::move(cb), [&]() {
        check_if_done(cb);
        auto error_handler = [this, cb](error_class ec, const std::string msg) {
            transaction_operation_failed err(ec, msg);
            switch (ec) {
                case FAIL_EXPIRY:
                    expiry_overtime_mode_ = true;
                    return op_completed_with_error(std::move(cb), err.expired());
                case FAIL_DOC_NOT_FOUND:
                case FAIL_DOC_ALREADY_EXISTS:
                case FAIL_CAS_MISMATCH:
                case FAIL_TRANSIENT:
                case FAIL_AMBIGUOUS:
                    return op_completed_with_error(std::move(cb), err.retry());
                case FAIL_HARD:
                    return op_completed_with_error(std::move(cb), err.no_rollback());
                default:
                    return op_completed_with_error(std::move(cb), err);
            }
        };
        if (check_expiry_pre_commit(STAGE_REMOVE, document.id().key())) {
            return error_handler(FAIL_EXPIRY, "transaction expired");
        }
        if (staged_mutations_->find_insert(document.id())) {
            error("cannot remove document {}, as it was inserted in this transaction", document.id());
            return op_completed_with_error(
              cb, transaction_operation_failed(FAIL_OTHER, "Cannot remove a document inserted in the same transaction"));
        }
        trace("removing {}", document);
        check_and_handle_blocking_transactions(
          document,
          forward_compat_stage::WWC_REMOVING,
          [this, document = std::move(document), cb = std::move(cb), error_handler](std::optional<transaction_operation_failed> err) {
              if (err) {
                  return op_completed_with_error(cb, *err);
              }
              select_atr_if_needed_unlocked(
                document.id(),
                [document = std::move(document), cb = std::move(cb), this, error_handler](std::optional<transaction_operation_failed> err) {
                    if (err) {
                        return op_completed_with_error(cb, *err);
                    }
                    if (auto ec = hooks_.before_staged_remove(this, document.id().key())) {
                        return error_handler(*ec, "before_staged_remove hook raised error");
                    }
                    trace("about to remove doc {} with cas {}", document.id(), document.cas());
                    auto req = create_staging_request(document, "remove");
                    req.cas.value = document.cas();
                    req.access_deleted = document.links().is_deleted();
                    overall_.cluster_ref().execute(
                      req,
                      [this, document = std::move(document), cb = std::move(cb), error_handler = std::move(error_handler)](
                        couchbase::operations::mutate_in_response resp) {
                          auto ec = error_class_from_response(resp);
                          if (!ec) {
                              ec = hooks_.after_staged_remove_complete(this, document.id().key());
                          }
                          if (!ec) {
                              trace("removed doc {} CAS={}, rc={}", document.id(), resp.cas.value, resp.ctx.ec.message());
                              // TODO: this copy...  can we do better?
                              transaction_get_result new_res = document;
                              new_res.cas(resp.cas.value);
                              staged_mutations_->add(staged_mutation(new_res, "", staged_mutation_type::REMOVE));
                              return op_completed_with_callback(cb);
                          }
                          return error_handler(*ec, resp.ctx.ec.message());
                      });
                });
          });
    });
}

void
attempt_context_impl::remove(transaction_get_result& document)
{
    auto barrier = std::make_shared<std::promise<void>>();
    auto f = barrier->get_future();
    remove(document, [barrier](std::optional<transaction_operation_failed> err) {
        if (err) {
            return barrier->set_exception(std::make_exception_ptr(*err));
        }
        barrier->set_value();
    });
    f.get();
}
void
attempt_context_impl::atr_commit()
{
    try {
        std::string prefix(ATR_FIELD_ATTEMPTS + "." + id() + ".");
        couchbase::operations::mutate_in_request req{ atr_id_.value() };
        req.specs.add_spec(protocol::subdoc_opcode::dict_upsert,
                           true,
                           false,
                           false,
                           prefix + ATR_FIELD_STATUS,
                           jsonify(attempt_state_name(attempt_state::COMMITTED)));
        req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, false, true, prefix + ATR_FIELD_START_COMMIT, mutate_in_macro::CAS);
        wrap_durable_request(req, config_);
        auto ec = error_if_expired_and_not_in_overtime(STAGE_ATR_COMMIT, {});
        if (ec) {
            throw client_error(*ec, "atr_abort check for expiry threw error");
        }
        if (!!(ec = hooks_.before_atr_commit(this))) {
            // for now, throw.  Later, if this is async, we will use error handler no doubt.
            throw client_error(*ec, "before_atr_commit hook raised error");
        }
        staged_mutations_->extract_to(prefix, req);
        auto barrier = std::make_shared<std::promise<result>>();
        auto f = barrier->get_future();
        trace("updating atr {}", req.id);
        overall_.cluster_ref().execute(req, [barrier](couchbase::operations::mutate_in_response resp) {
            barrier->set_value(result::create_from_subdoc_response(resp));
        });
        auto res = wrap_operation_future(f);
        ec = hooks_.after_atr_commit(this);
        if (ec) {
            throw client_error(*ec, "after_atr_commit hook raised error");
        }
        state(attempt_state::COMMITTED);
    } catch (const client_error& e) {
        error_class ec = e.ec();
        switch (ec) {
            case FAIL_EXPIRY:
                expiry_overtime_mode_ = true;
                throw transaction_operation_failed(ec, e.what()).expired();
            case FAIL_AMBIGUOUS:
                debug("atr_commit got FAIL_AMBIGUOUS, resolving ambiguity...");
                try {
                    return retry_op<void>([&]() { return atr_commit_ambiguity_resolution(); });
                } catch (const retry_atr_commit& e) {
                    debug("ambiguity resolution will retry atr_commit");
                    throw retry_operation(e.what());
                }
            case FAIL_TRANSIENT:
                throw transaction_operation_failed(ec, e.what()).retry();
            case FAIL_HARD:
                throw transaction_operation_failed(ec, e.what()).no_rollback();
            default:
                error("failed to commit transaction {}, attempt {}, with error {}", transaction_id(), id(), e.what());
                throw transaction_operation_failed(ec, e.what());
        }
    }
}

void
attempt_context_impl::atr_commit_ambiguity_resolution()
{
    try {
        auto ec = error_if_expired_and_not_in_overtime(STAGE_ATR_COMMIT_AMBIGUITY_RESOLUTION, {});
        if (ec) {
            throw client_error(*ec, "atr_commit_ambiguity_resolution raised error");
        }
        if (!!(ec = hooks_.before_atr_commit_ambiguity_resolution(this))) {
            throw client_error(*ec, "before_atr_commit_ambiguity_resolution hook threw error");
        }
        std::string prefix(ATR_FIELD_ATTEMPTS + "." + id() + ".");
        couchbase::operations::lookup_in_request req{ atr_id_.value() };
        req.specs.add_spec(protocol::subdoc_opcode::get, true, prefix + ATR_FIELD_STATUS);
        wrap_request(req, config_);
        auto barrier = std::make_shared<std::promise<result>>();
        auto f = barrier->get_future();
        overall_.cluster_ref().execute(req, [barrier](couchbase::operations::lookup_in_response resp) {
            barrier->set_value(result::create_from_subdoc_response(resp));
        });
        auto res = wrap_operation_future(f);
        auto atr_status = attempt_state_value(res.values[0].content_as<std::string>());
        switch (atr_status) {
            case attempt_state::COMPLETED:
                return;
            case attempt_state::ABORTED:
            case attempt_state::ROLLED_BACK:
                // rolled back by another process?
                throw transaction_operation_failed(FAIL_OTHER, "transaction rolled back externally").no_rollback();
            default:
                // still pending - so we can safely retry
                throw retry_atr_commit("atr still pending, retry atr_commit");
        }
    } catch (const client_error& e) {
        error_class ec = e.ec();
        switch (ec) {
            case FAIL_EXPIRY:
                expiry_overtime_mode_ = true;
                throw transaction_operation_failed(ec, e.what()).no_rollback().ambiguous();
            case FAIL_HARD:
                throw transaction_operation_failed(ec, e.what()).no_rollback();
            case FAIL_TRANSIENT:
            case FAIL_OTHER:
                throw retry_operation(e.what());
            case FAIL_PATH_NOT_FOUND:
                throw transaction_operation_failed(FAIL_OTHER, "transaction rolled back externally").no_rollback();
            default:
                throw transaction_operation_failed(ec, e.what()).no_rollback();
        }
    }
}

void
attempt_context_impl::atr_complete()
{
    try {
        result atr_res;
        auto ec = hooks_.before_atr_complete(this);
        if (ec) {
            throw client_error(*ec, "before_atr_complete hook threw error");
        }
        // if we have expired (and not in overtime mode), just raise the final error.
        if (!!(ec = error_if_expired_and_not_in_overtime(STAGE_ATR_COMPLETE, {}))) {
            throw client_error(*ec, "atr_complete threw error");
        }
        debug("removing attempt {} from atr", atr_id_.value());
        std::string prefix(ATR_FIELD_ATTEMPTS + "." + id());
        couchbase::operations::mutate_in_request req{ atr_id_.value() };
        req.specs.add_spec(protocol::subdoc_opcode::remove, true, prefix);
        wrap_durable_request(req, config_);
        auto barrier = std::make_shared<std::promise<result>>();
        auto f = barrier->get_future();
        overall_.cluster_ref().execute(req, [barrier](couchbase::operations::mutate_in_response resp) {
            barrier->set_value(result::create_from_subdoc_response(resp));
        });
        wrap_operation_future(f);
        ec = hooks_.after_atr_complete(this);
        if (ec) {
            throw client_error(*ec, "after_atr_complete hook threw error");
        }
        state(attempt_state::COMPLETED);
    } catch (const client_error& er) {
        error_class ec = er.ec();
        switch (ec) {
            case FAIL_HARD:
                throw transaction_operation_failed(ec, er.what()).no_rollback().failed_post_commit();
            default:
                info("ignoring error in atr_complete {}", er.what());
        }
    }
}

void
attempt_context_impl::commit()
{
    debug("waiting on ops to finish before committing...");
    op_list_.wait_and_block_ops();
    debug("commit {}", id());
    existing_error();
    if (check_expiry_pre_commit(STAGE_BEFORE_COMMIT, {})) {
        throw transaction_operation_failed(FAIL_EXPIRY, "transaction expired").expired();
    }
    if (atr_id_ && !atr_id_->key().empty() && !is_done_) {
        retry_op_exp<void>([&]() { atr_commit(); });
        staged_mutations_->commit(*this);
        atr_complete();
        is_done_ = true;
    } else {
        // no mutation, no need to commit
        if (!is_done_) {
            debug("calling commit on attempt that has got no mutations, skipping");
            is_done_ = true;
            return;
        } else {
            // do not rollback or retry
            throw transaction_operation_failed(FAIL_OTHER, "calling commit on attempt that is already completed").no_rollback();
        }
    }
}

void
attempt_context_impl::atr_abort()
{
    try {
        auto ec = error_if_expired_and_not_in_overtime(STAGE_ATR_ABORT, {});
        if (ec) {
            throw client_error(*ec, "atr_abort check for expiry threw error");
        }
        if (!!(ec = hooks_.before_atr_aborted(this))) {
            throw client_error(*ec, "before_atr_aborted hook threw error");
        }
        std::string prefix(ATR_FIELD_ATTEMPTS + "." + id() + ".");
        couchbase::operations::mutate_in_request req{ atr_id_.value() };
        req.specs.add_spec(protocol::subdoc_opcode::dict_upsert,
                           true,
                           true,
                           false,
                           prefix + ATR_FIELD_STATUS,
                           jsonify(attempt_state_name(attempt_state::ABORTED)));
        req.specs.add_spec(
          protocol::subdoc_opcode::dict_upsert, true, true, true, prefix + ATR_FIELD_TIMESTAMP_ROLLBACK_START, mutate_in_macro::CAS);
        staged_mutations_->extract_to(prefix, req);
        wrap_durable_request(req, config_);
        auto barrier = std::make_shared<std::promise<result>>();
        auto f = barrier->get_future();
        overall_.cluster_ref().execute(req, [barrier](couchbase::operations::mutate_in_response resp) {
            barrier->set_value(result::create_from_subdoc_response(resp));
        });
        wrap_operation_future(f);
        state(attempt_state::ABORTED);
        ec = hooks_.after_atr_aborted(this);
        if (ec) {
            throw client_error(*ec, "after_atr_aborted hook threw error");
        }
        debug("rollback completed atr abort phase");
    } catch (const client_error& e) {
        auto ec = e.ec();
        trace("atr_abort got {} {}", ec, e.what());
        if (expiry_overtime_mode_.load()) {
            debug("atr_abort got error {} while in overtime mode", e.what());
            throw transaction_operation_failed(FAIL_EXPIRY, std::string("expired in atr_abort with {} ") + e.what())
              .no_rollback()
              .expired();
        }
        debug("atr_abort got error {}", ec);
        switch (ec) {
            case FAIL_EXPIRY:
                expiry_overtime_mode_ = true;
                throw retry_operation("expired, setting overtime mode and retry atr_abort");
            case FAIL_PATH_NOT_FOUND:
                throw transaction_operation_failed(ec, e.what()).no_rollback().cause(ACTIVE_TRANSACTION_RECORD_ENTRY_NOT_FOUND);
            case FAIL_DOC_NOT_FOUND:
                throw transaction_operation_failed(ec, e.what()).no_rollback().cause(ACTIVE_TRANSACTION_RECORD_NOT_FOUND);
            case FAIL_ATR_FULL:
                throw transaction_operation_failed(ec, e.what()).no_rollback().cause(ACTIVE_TRANSACTION_RECORD_FULL);
            case FAIL_HARD:
                throw transaction_operation_failed(ec, e.what()).no_rollback();
            default:
                throw retry_operation("retry atr_abort");
        }
    }
}

void
attempt_context_impl::atr_rollback_complete()
{
    try {
        auto ec = error_if_expired_and_not_in_overtime(STAGE_ATR_ROLLBACK_COMPLETE, std::nullopt);
        if (ec) {
            throw client_error(*ec, "atr_rollback_complete raised error");
        }
        if (!!(ec = hooks_.before_atr_rolled_back(this))) {
            throw client_error(*ec, "before_atr_rolled_back hook threw error");
        }
        std::string prefix(ATR_FIELD_ATTEMPTS + "." + id());
        couchbase::operations::mutate_in_request req{ atr_id_.value() };
        req.specs.add_spec(protocol::subdoc_opcode::remove, true, prefix);
        wrap_durable_request(req, config_);
        auto barrier = std::make_shared<std::promise<result>>();
        auto f = barrier->get_future();
        overall_.cluster_ref().execute(req, [barrier](couchbase::operations::mutate_in_response resp) {
            barrier->set_value(result::create_from_subdoc_response(resp));
        });
        wrap_operation_future(f);
        state(attempt_state::ROLLED_BACK);
        ec = hooks_.after_atr_rolled_back(this);
        if (ec) {
            throw client_error(*ec, "after_atr_rolled_back hook threw error");
        }
        is_done_ = true;

    } catch (const client_error& e) {
        auto ec = e.ec();
        if (expiry_overtime_mode_.load()) {
            debug("atr_rollback_complete error while in overtime mode {}", e.what());
            throw transaction_operation_failed(FAIL_EXPIRY, std::string("expired in atr_rollback_complete with {} ") + e.what())
              .no_rollback()
              .expired();
        }
        debug("atr_rollback_complete got error {}", ec);
        switch (ec) {
            case FAIL_DOC_NOT_FOUND:
            case FAIL_PATH_NOT_FOUND:
                debug("atr {} not found, ignoring", atr_id_->key());
                is_done_ = true;
                break;
            case FAIL_ATR_FULL:
                debug("atr {} full!", atr_id_->key());
                throw retry_operation(e.what());
            case FAIL_HARD:
                throw transaction_operation_failed(ec, e.what()).no_rollback();
            case FAIL_EXPIRY:
                debug("timed out writing atr {}", atr_id_->key());
                throw transaction_operation_failed(ec, e.what()).no_rollback().expired();
            default:
                debug("retrying atr_rollback_complete");
                throw retry_operation(e.what());
        }
    }
}

void
attempt_context_impl::rollback()
{
    op_list_.wait_and_block_ops();
    debug("rolling back {}", id());
    // check for expiry
    check_expiry_during_commit_or_rollback(STAGE_ROLLBACK, std::nullopt);
    if (!atr_id_ || atr_id_->key().empty() || state() == attempt_state::NOT_STARTED) {
        // TODO: check this, but if we try to rollback an empty txn, we should
        // prevent a subsequent commit
        debug("rollback called on txn with no mutations");
        is_done_ = true;
        return;
    }
    if (is_done()) {
        std::string msg("Transaction already done, cannot rollback");
        error(msg);
        // need to raise a FAIL_OTHER which is not retryable or rollback-able
        throw transaction_operation_failed(FAIL_OTHER, msg).no_rollback();
    }
    try {
        // (1) atr_abort
        retry_op_exp<void>([&] { atr_abort(); });
        // (2) rollback staged mutations
        staged_mutations_->rollback(*this);
        debug("rollback completed unstaging docs");

        // (3) atr_rollback
        retry_op_exp<void>([&] { atr_rollback_complete(); });
    } catch (const client_error& e) {
        error_class ec = e.ec();
        error("rollback transaction {}, attempt {} fail with error {}", transaction_id(), id(), e.what());
        if (ec == FAIL_HARD) {
            throw transaction_operation_failed(ec, e.what()).no_rollback();
        }
    }
}

bool
attempt_context_impl::has_expired_client_side(std::string place, std::optional<const std::string> doc_id)
{
    bool over = overall_.has_expired_client_side(config_);
    bool hook = hooks_.has_expired_client_side(this, place, doc_id);
    if (over) {
        debug("{} expired in {}", id(), place);
    }
    if (hook) {
        debug("{} fake expiry in {}", id(), place);
    }
    return over || hook;
}

bool
attempt_context_impl::check_expiry_pre_commit(std::string stage, std::optional<const std::string> doc_id)
{
    if (has_expired_client_side(stage, std::move(doc_id))) {
        debug("{} has expired in stage {}, entering expiry-overtime mode - will make one attempt to rollback", id(), stage);

        // [EXP-ROLLBACK] Combo of setting this mode and throwing AttemptExpired will result in an attempt to rollback, which will
        // ignore expiries, and bail out if anything fails
        expiry_overtime_mode_ = true;
        return true;
    }
    return false;
}

std::optional<error_class>
attempt_context_impl::error_if_expired_and_not_in_overtime(const std::string& stage, std::optional<const std::string> doc_id)
{
    if (expiry_overtime_mode_.load()) {
        debug("not doing expired check in {} as already in expiry-overtime", stage);
        return {};
    }
    if (has_expired_client_side(stage, std::move(doc_id))) {
        debug("expired in {}", stage);
        return FAIL_EXPIRY;
    }
    return {};
}

void
attempt_context_impl::check_expiry_during_commit_or_rollback(const std::string& stage, std::optional<const std::string> doc_id)
{
    // [EXP-COMMIT-OVERTIME]
    if (!expiry_overtime_mode_.load()) {
        if (has_expired_client_side(stage, std::move(doc_id))) {
            debug("{} has expired in stage {}, entering expiry-overtime mode (one attempt to complete commit)", id(), stage);
            expiry_overtime_mode_ = true;
        }
    } else {
        debug("{} ignoring expiry in stage {}  as in expiry-overtime mode", id(), stage);
    }
}
template<typename Handler>
void
attempt_context_impl::set_atr_pending_locked(const couchbase::document_id& id, std::unique_lock<std::mutex>&& lock, Handler&& fn)
{
    try {
        if (staged_mutations_->empty()) {
            std::string prefix(ATR_FIELD_ATTEMPTS + "." + this->id() + ".");
            if (!atr_id_) {
                return fn(transaction_operation_failed(FAIL_OTHER, std::string("ATR ID is not initialized")));
            }
            if (auto ec = error_if_expired_and_not_in_overtime(STAGE_ATR_PENDING, {})) {
                return fn(transaction_operation_failed(*ec, "transaction expired setting ATR").expired());
            }
            auto error_handler = [this, &lock, fn](error_class ec, const std::string& message, const couchbase::document_id& id) {
                transaction_operation_failed err(ec, message);
                trace("got {} trying to set atr to pending", message);
                if (expiry_overtime_mode_.load()) {
                    return fn(err.no_rollback().expired());
                }
                switch (ec) {
                    case FAIL_EXPIRY:
                        expiry_overtime_mode_ = true;
                        // this should trigger rollback (unlike the above when already in overtime mode)
                        return fn(err.expired());
                    case FAIL_ATR_FULL:
                        return fn(err);
                    case FAIL_PATH_ALREADY_EXISTS:
                        // assuming this got resolved, moving on as if ok
                        return fn(std::nullopt);
                    case FAIL_AMBIGUOUS:
                        // Retry just this
                        overall_.retry_delay();
                        // keep it locked!
                        debug("got {}, retrying set atr pending", ec);
                        return set_atr_pending_locked(id, std::move(lock), fn);
                    case FAIL_TRANSIENT:
                        // Retry txn
                        return fn(err.retry());
                    case FAIL_HARD:
                        return fn(err.no_rollback());
                    default:
                        return fn(err);
                }
            };
            auto ec = hooks_.before_atr_pending(this);
            if (ec) {
                return error_handler(*ec, "before_atr_pending hook raised error", id);
            }
            debug("updating atr {}", atr_id_.value());
            couchbase::operations::mutate_in_request req{ atr_id_.value() };

            req.specs.add_spec(
              protocol::subdoc_opcode::dict_add, true, true, false, prefix + ATR_FIELD_TRANSACTION_ID, jsonify(overall_.transaction_id()));
            req.specs.add_spec(protocol::subdoc_opcode::dict_add,
                               true,
                               true,
                               false,
                               prefix + ATR_FIELD_STATUS,
                               jsonify(attempt_state_name(attempt_state::PENDING)));
            req.specs.add_spec(
              protocol::subdoc_opcode::dict_add, true, true, true, prefix + ATR_FIELD_START_TIMESTAMP, mutate_in_macro::CAS);
            req.specs.add_spec(protocol::subdoc_opcode::dict_add,
                               true,
                               true,
                               false,
                               prefix + ATR_FIELD_EXPIRES_AFTER_MSECS,
                               jsonify(std::chrono::duration_cast<std::chrono::milliseconds>(config_.expiration_time()).count()));
            req.store_semantics = protocol::mutate_in_request_body::store_semantics_type::upsert;

            wrap_durable_request(req, config_);
            overall_.cluster_ref().execute(req, [this, fn, error_handler](couchbase::operations::mutate_in_response resp) {
                auto ec = error_class_from_response(resp);
                if (!ec) {
                    ec = hooks_.after_atr_pending(this);
                }
                if (!ec) {
                    state(attempt_state::PENDING);
                    debug("set ATR {} to Pending, got CAS (start time) {}", atr_id_.value(), resp.cas.value);
                    return fn(std::nullopt);
                }
                return error_handler(*ec, resp.ctx.ec.message(), resp.ctx.id);
            });
        }
    } catch (const std::exception& e) {
        error("unexpected error setting atr pending {}", e.what());
        return fn(transaction_operation_failed(FAIL_OTHER, "unexpected error setting atr pending"));
    }
}

staged_mutation*
attempt_context_impl::check_for_own_write(const couchbase::document_id& id)
{
    staged_mutation* own_replace = staged_mutations_->find_replace(id);
    if (own_replace) {
        return own_replace;
    }
    staged_mutation* own_insert = staged_mutations_->find_insert(id);
    if (own_insert) {
        return own_insert;
    }
    return nullptr;
}

template<typename Handler>
void
attempt_context_impl::check_if_done(Handler& cb)
{
    if (is_done_) {
        return op_completed_with_error(
          cb,
          transaction_operation_failed(FAIL_OTHER, "Cannot perform operations after transaction has been committed or rolled back")
            .no_rollback());
    }
}
template<typename Handler>
void
attempt_context_impl::do_get(const couchbase::document_id& id, Handler&& cb)
{
    try {
        if (check_expiry_pre_commit(STAGE_GET, id.key())) {
            return cb(FAIL_EXPIRY, std::nullopt);
        }

        staged_mutation* own_write = check_for_own_write(id);
        if (own_write) {
            debug("found own-write of mutated doc {}", id);
            return cb(std::nullopt, transaction_get_result::create_from(own_write->doc(), own_write->content()));
        }
        staged_mutation* own_remove = staged_mutations_->find_remove(id);
        if (own_remove) {
            debug("found own-write of removed doc {}", id);
            return cb(std::nullopt, std::nullopt);
        }

        auto ec = hooks_.before_doc_get(this, id.key());
        if (ec) {
            return cb(ec, std::nullopt);
        }

        get_doc(id, [this, id, cb = std::move(cb)](std::optional<error_class> ec, std::optional<transaction_get_result> doc) {
            if (!ec && !doc) {
                // it just isn't there.
                return cb(std::nullopt, std::nullopt);
            }
            if (!ec) {
                if (doc->links().is_document_in_transaction()) {
                    debug("doc {} in transaction", *doc);
                    couchbase::document_id doc_atr_id{ doc->links().atr_bucket_name().value(),
                                                       doc->links().atr_scope_name().value(),
                                                       doc->links().atr_collection_name().value(),
                                                       doc->links().atr_id().value() };
                    active_transaction_record::get_atr(
                      cluster_ref(),
                      doc_atr_id,
                      [this, doc, cb = std::move(cb)](std::error_code ec, std::optional<active_transaction_record> atr) {
                          if (!ec && atr) {
                              active_transaction_record& atr_doc = atr.value();
                              std::optional<atr_entry> entry;
                              for (auto& e : atr_doc.entries()) {
                                  if (doc->links().staged_attempt_id().value() == e.attempt_id()) {
                                      entry.emplace(e);
                                      break;
                                  }
                              }
                              bool ignore_doc = false;
                              auto content = doc->content<std::string>();
                              if (entry) {
                                  if (doc->links().staged_attempt_id() && entry->attempt_id() == this->id()) {
                                      // Attempt is reading its own writes
                                      // This is here as backup, it should be returned from the in-memory cache instead
                                      content = doc->links().staged_content();
                                  } else {
                                      auto err = forward_compat::check(forward_compat_stage::GETS_READING_ATR, entry->forward_compat());
                                      if (err) {
                                          return cb(err->ec(), std::nullopt);
                                      }
                                      switch (entry->state()) {
                                          case attempt_state::COMMITTED:
                                              if (doc->links().is_document_being_removed()) {
                                                  ignore_doc = true;
                                              } else {
                                                  content = doc->links().staged_content();
                                              }
                                              break;
                                          default:
                                              if (doc->content<std::string>().empty()) {
                                                  // This document is being inserted, so should not be visible yet
                                                  ignore_doc = true;
                                              }
                                              break;
                                      }
                                  }
                              } else {
                                  // Don't know if transaction was committed or rolled back. Should not happen as ATR should stick
                                  // around long enough
                                  if (content.empty()) {
                                      // This document is being inserted, so should not be visible yet
                                      ignore_doc = true;
                                  }
                              }
                              if (ignore_doc) {
                                  return cb(std::nullopt, std::nullopt);
                              } else {
                                  return cb(std::nullopt, transaction_get_result::create_from(*doc, content));
                              }
                          } else {
                              // failed to get the ATR
                              if (doc->content<nlohmann::json>().empty()) {
                                  // this document is being inserted, so should not be visible yet
                                  return cb(std::nullopt, std::nullopt);
                              }
                              return cb(std::nullopt, doc);
                          }
                      });
                } else {
                    if (doc->links().is_deleted()) {
                        debug("doc not in txn, and is_deleted, so not returning it.");
                        // doc has been deleted, not in txn, so don't return it
                        return cb(std::nullopt, std::nullopt);
                    }
                    return cb(std::nullopt, doc);
                }
            } else {
                return cb(ec, std::nullopt);
            }
        });

    } catch (const transaction_operation_failed& e) {
        throw;
    } catch (const std::exception& ex) {
        std::ostringstream stream;
        stream << "got error while getting doc " << id.key() << ": " << ex.what();
        throw transaction_operation_failed(FAIL_OTHER, ex.what());
    }
}

void
attempt_context_impl::get_doc(const couchbase::document_id& id,
                              std::function<void(std::optional<error_class>, std::optional<transaction_get_result>)>&& cb)
{
    couchbase::operations::lookup_in_request req{ id };
    req.specs.add_spec(protocol::subdoc_opcode::get, true, ATR_ID);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, TRANSACTION_ID);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, ATTEMPT_ID);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, STAGED_DATA);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, ATR_BUCKET_NAME);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, ATR_COLL_NAME);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, TRANSACTION_RESTORE_PREFIX_ONLY);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, TYPE);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, "$document");
    req.specs.add_spec(protocol::subdoc_opcode::get, true, CRC32_OF_STAGING);
    req.specs.add_spec(protocol::subdoc_opcode::get, true, FORWARD_COMPAT);
    req.specs.add_spec(protocol::subdoc_opcode::get_doc, false, "");
    req.access_deleted = true;
    wrap_request(req, config_);
    try {
        overall_.cluster_ref().execute(req, [this, id, cb = std::move(cb)](couchbase::operations::lookup_in_response resp) {
            auto ec = error_class_from_response(resp);
            if (ec) {
                trace("get_doc got error {}", *ec);
                switch (*ec) {
                    case FAIL_PATH_NOT_FOUND:
                        cb(*ec, transaction_get_result::create_from(resp));
                    default:
                        cb(*ec, std::nullopt);
                }
            } else {
                cb({}, transaction_get_result::create_from(resp));
            }
        });
    } catch (const std::exception& e) {
        cb(FAIL_OTHER, std::nullopt);
    }
}

template<typename Handler, typename Delay>
void
attempt_context_impl::create_staged_insert_error_handler(const couchbase::document_id& id,
                                                         const std::string& content,
                                                         uint64_t cas,
                                                         Delay&& delay,
                                                         Handler&& cb,
                                                         error_class ec,
                                                         const std::string& message)
{
    trace("create_staged_insert got error class {}", ec);
    if (expiry_overtime_mode_.load()) {
        return op_completed_with_error(cb, transaction_operation_failed(FAIL_EXPIRY, "attempt timed out").expired());
    }
    switch (ec) {
        case FAIL_EXPIRY:
            expiry_overtime_mode_ = true;
            return op_completed_with_error(cb, transaction_operation_failed(ec, "attempt timed-out").expired());
        case FAIL_TRANSIENT:
            return op_completed_with_error(cb, transaction_operation_failed(ec, "transient error in insert").retry());
        case FAIL_AMBIGUOUS:
            debug("FAIL_AMBIGUOUS in create_staged_insert, retrying");
            delay();
            return create_staged_insert(id, content, cas, delay, cb);
        case FAIL_OTHER:
            return op_completed_with_error(cb, transaction_operation_failed(ec, "error in create_staged_insert"));
        case FAIL_HARD:
            return op_completed_with_error(cb, transaction_operation_failed(ec, "error in create_staged_insert").no_rollback());
        case FAIL_DOC_ALREADY_EXISTS:
        case FAIL_CAS_MISMATCH: {
            // special handling for doc already existing
            debug("found existing doc {}, may still be able to insert", id);
            auto error_handler = [this, id, content, cb](error_class ec) {
                trace("after a CAS_MISMATCH or DOC_ALREADY_EXISTS, then got error {} in create_staged_insert", ec);
                if (expiry_overtime_mode_.load()) {
                    return op_completed_with_error(std::move(cb), transaction_operation_failed(FAIL_EXPIRY, "attempt timed out").expired());
                }
                switch (ec) {
                    case FAIL_EXPIRY:
                        expiry_overtime_mode_ = true;
                        return op_completed_with_error(std::move(cb), transaction_operation_failed(ec, "attempt timed-out").expired());
                    case FAIL_TRANSIENT:
                    case FAIL_PATH_NOT_FOUND:
                        debug("transient error trying to get doc in insert - retrying txn");
                        return op_completed_with_error(std::move(cb),
                                                       transaction_operation_failed(ec, "error handling found doc in insert").retry());
                    case FAIL_OTHER:
                        return op_completed_with_error(std::move(cb),
                                                       transaction_operation_failed(ec, "failed getting doc in create_staged_insert"));
                    case FAIL_HARD:
                        return op_completed_with_error(
                          std::move(cb), transaction_operation_failed(ec, "failed getting doc in create_staged_insert").no_rollback());
                    default:
                        return op_completed_with_error(
                          std::move(cb), transaction_operation_failed(ec, "failed getting doc in create_staged_insert").retry());
                }
            };
            auto err = hooks_.before_get_doc_in_exists_during_staged_insert(this, id.key());
            if (err) {
                trace("before_get_doc_in_exists_during_staged_insert hook raised {}", *err);
                return error_handler(*err);
            }
            return get_doc(
              id, [this, id, content, cb, error_handler, delay](std::optional<error_class> ec, std::optional<transaction_get_result> doc) {
                  if (!ec) {
                      if (doc) {
                          debug("document {} exists, is_in_transaction {}, is_deleted {} ",
                                doc->id(),
                                doc->links().is_document_in_transaction(),
                                doc->links().is_deleted());
                          auto err = forward_compat::check(forward_compat_stage::WWC_INSERTING_GET, doc->links().forward_compat());
                          if (err) {
                              return op_completed_with_error(std::move(cb), *err);
                          }
                          if (!doc->links().is_document_in_transaction() && doc->links().is_deleted()) {
                              // it is just a deleted doc, so we are ok.  Let's try again, but with the cas
                              debug("create staged insert found existing deleted doc, retrying with cas {}", doc->cas());
                              delay();
                              return create_staged_insert(id, content, doc->cas(), delay, cb);
                          }
                          if (!doc->links().is_document_in_transaction()) {
                              // doc was inserted outside txn elsewhere
                              trace("doc {} not in txn - was inserted outside tnx", id);
                              return op_completed_with_error(
                                std::move(cb), transaction_operation_failed(FAIL_DOC_ALREADY_EXISTS, "document already exists"));
                          }
                          // CBD-3787 - Only a staged insert is ok to overwrite
                          if (doc->links().op() && *doc->links().op() != "insert") {
                              return op_completed_with_error(
                                std::move(cb),
                                transaction_operation_failed(FAIL_DOC_ALREADY_EXISTS, "doc exists, not a staged insert")
                                  .cause(DOCUMENT_EXISTS_EXCEPTION));
                          }
                          check_and_handle_blocking_transactions(
                            *doc,
                            forward_compat_stage::WWC_INSERTING,
                            [this, id, content, doc, cb, delay](std::optional<transaction_operation_failed> err) {
                                if (err) {
                                    return op_completed_with_error(cb, *err);
                                }
                                debug("doc ok to overwrite, retrying create_staged_insert with cas {}", doc->cas());
                                delay();
                                return create_staged_insert(id, content, doc->cas(), delay, cb);
                            });
                      } else {
                          // no doc now, just retry entire txn
                          trace("got {} from get_doc in exists during staged insert", *ec);
                          return op_completed_with_error(
                            cb,
                            transaction_operation_failed(FAIL_DOC_NOT_FOUND, "insert failed as the doc existed, but now seems to not exist")
                              .retry());
                      }
                  } else {
                      return error_handler(*ec);
                  }
              });
        } break;
        default:
            return op_completed_with_error(std::move(cb), transaction_operation_failed(ec, "failed in create_staged_insert").retry());
    }
}

template<typename Handler, typename Delay>
void
attempt_context_impl::create_staged_insert(const couchbase::document_id& id,
                                           const std::string& content,
                                           uint64_t cas,
                                           Delay&& delay,
                                           Handler&& cb)
{
    auto ec = error_if_expired_and_not_in_overtime(STAGE_CREATE_STAGED_INSERT, id.key());
    if (ec) {
        return create_staged_insert_error_handler(
          id, content, cas, std::move(delay), cb, *ec, "create_staged_insert expired and not in overtime");
    }

    ec = hooks_.before_staged_insert(this, id.key());
    if (ec) {
        return create_staged_insert_error_handler(id, content, cas, std::move(delay), cb, *ec, "before_staged_insert hook threw error");
    }
    debug("about to insert staged doc {} with cas {}", id, cas);
    couchbase::operations::mutate_in_request req{ id };
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, TRANSACTION_ID, jsonify(overall_.transaction_id()));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, ATTEMPT_ID, jsonify(this->id()));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, ATR_ID, jsonify(atr_id()));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, STAGED_DATA, content);
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, ATR_BUCKET_NAME, jsonify(id.bucket()));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, ATR_COLL_NAME, jsonify(collection_spec_from_id(id)));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, false, TYPE, jsonify("insert"));
    req.specs.add_spec(protocol::subdoc_opcode::dict_upsert, true, true, true, CRC32_OF_STAGING, mutate_in_macro::VALUE_CRC_32C);
    req.access_deleted = true;
    req.create_as_deleted = true;
    req.cas.value = cas;
    req.store_semantics = cas == 0 ? protocol::mutate_in_request_body::store_semantics_type::insert
                                   : protocol::mutate_in_request_body::store_semantics_type::replace;
    wrap_durable_request(req, config_);
    overall_.cluster_ref().execute(req, [this, id, content, cas, cb, delay](couchbase::operations::mutate_in_response resp) {
        auto ec = hooks_.after_staged_insert_complete(this, id.key());
        if (ec) {
            return create_staged_insert_error_handler(id, content, cas, std::move(delay), cb, *ec, "after_staged_insert hook threw error");
        }
        if (!resp.ctx.ec) {
            debug("inserted doc {} CAS={}, {}", id, resp.cas.value, resp.ctx.ec.message());

            // TODO: clean this up (do most of this in transactions_document(...))
            transaction_links links(atr_id_->key(),
                                    id.bucket(),
                                    id.scope(),
                                    id.collection(),
                                    overall_.transaction_id(),
                                    this->id(),
                                    content,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::string("insert"),
                                    std::nullopt,
                                    true);
            transaction_get_result out(id, content, resp.cas.value, links, std::nullopt);
            staged_mutations_->add(staged_mutation(out, content, staged_mutation_type::INSERT));
            return op_completed_with_callback(cb, out);
        }
        ec = error_class_from_response(resp);
        return create_staged_insert_error_handler(id, content, cas, std::move(delay), cb, *ec, resp.ctx.ec.message());
    });
}

} // namespace couchbase::transactions
