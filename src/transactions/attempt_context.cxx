#include <iostream>

#include <couchbase/client/collection.hxx>
#include <couchbase/transactions/attempt_context.hxx>
#include <couchbase/transactions/uid_generator.hxx>
#include <couchbase/transactions/attempt_state.hxx>
#include <couchbase/transactions/transaction_fields.hxx>
#include <couchbase/transactions/logging.hxx>

#include "atr_ids.hxx"

namespace tx = couchbase::transactions;

static lcb_DURABILITY_LEVEL durability(const tx::configuration &config)
{
    switch (config.durability_level()) {
        case tx::NONE:
            return LCB_DURABILITYLEVEL_NONE;
        case tx::MAJORITY:
            return LCB_DURABILITYLEVEL_MAJORITY;
        case tx::MAJORITY_AND_PERSIST_ON_MASTER:
            return LCB_DURABILITYLEVEL_MAJORITY_AND_PERSIST_ON_MASTER;
        case tx::PERSIST_TO_MAJORITY:
            return LCB_DURABILITYLEVEL_PERSIST_TO_MAJORITY;
    }
    throw std::runtime_error("unknown durability: " + std::to_string(config.durability_level()));
}

tx::attempt_context::attempt_context(tx::transaction_context &transaction_ctx, const tx::configuration &config)
    : txctx_(transaction_ctx), config_(config), state_(attempt_state::NOT_STARTED), id_(uid_generator::next()), atr_collection_(nullptr),
      is_done_(false)
{
}

void tx::attempt_context::init_atr_if_needed(couchbase::collection *collection, const std::string &id)
{
    if (atr_id_) {
        int vbucket_id = atr_ids::vbucket_for_key(id);
        atr_id_.emplace(atr_ids::atr_id_for_vbucket(vbucket_id));
        atr_collection_ = collection;
        state_ = attempt_state::PENDING;
        LOG(txctx_, info) << "first mutated doc in transaction is \"" << id << "\" on vbucket " << vbucket_id << ", so using atr \""
                          << atr_id_.value() << "\"";
    }
}

bool tx::attempt_context::is_done()
{
    return is_done_;
}

std::optional<tx::transaction_document> tx::attempt_context::get(collection *collection, const std::string &id)
{
    staged_mutation *mutation = staged_mutations_.find_replace(collection, id);
    if (mutation == nullptr) {
        mutation = staged_mutations_.find_insert(collection, id);
    }
    if (mutation) {
        return transaction_document(*collection, id, mutation->content(), 0, transaction_document_status::OWN_WRITE,
                                    transaction_links(mutation->doc().links()));
    }
    mutation = staged_mutations_.find_remove(collection, id);
    if (mutation) {
        throw std::runtime_error(std::string("not found"));
    }

    std::optional<transaction_document> out;
    LOG(txctx_, trace) << "getting doc " << id;
    const result &res = collection->lookup_in(id, { lookup_in_spec::get(ATR_ID).xattr(), lookup_in_spec::get(STAGED_VERSION).xattr(),
                                                    lookup_in_spec::get(STAGED_DATA).xattr(), lookup_in_spec::get(ATR_BUCKET_NAME).xattr(),
                                                    lookup_in_spec::get(ATR_SCOPE_NAME).xattr(), lookup_in_spec::get(ATR_COLL_NAME).xattr(),
                                                    lookup_in_spec::fulldoc_get() });
    if (res.rc == LCB_SUCCESS || res.rc == LCB_SUBDOC_MULTI_FAILURE) {
        std::string atr_id = res.values[0]->asString();
        std::string staged_version = res.values[1]->asString();
        folly::dynamic staged_data = *res.values[2];
        std::string atr_bucket_name = res.values[3]->asString();
        std::string atr_scope_name = res.values[4]->asString();
        std::string atr_coll_name = res.values[5]->asString();
        folly::dynamic content = *res.values[6];

        transaction_document doc(*collection, id, content, res.cas, transaction_document_status::NORMAL,
                                 transaction_links(atr_id, atr_bucket_name, atr_scope_name, atr_coll_name, content, id));

        if (doc.links().is_document_in_transaction()) {
            const result &atr_res = collection->lookup_in(doc.links().atr_id(), { lookup_in_spec::get(ATR_FIELD_ATTEMPTS).xattr() });
            if (atr_res.rc != LCB_KEY_ENOENT && atr_res.values[0] != nullptr) {
                std::string err;
                const folly::dynamic &atr = *atr_res.values[0];
                const folly::dynamic &entry = atr[id_];
                if (entry == nullptr) {
                    // Don't know if txn was committed or rolled back.  Should not happen as ATR record should stick around long enough.
                    doc.status(transaction_document_status::AMBIGUOUS);
                    if (doc.content() == nullptr) {
                        throw std::runtime_error(std::string("not found"));
                    }
                } else {
                    if (doc.links().staged_version().empty()) {
                        if (entry["status"] == "COMMITTED") {
                            if (doc.links().is_document_being_removed()) {
                                throw std::runtime_error(std::string("not found"));
                            } else {
                                doc.content(doc.links().staged_content());
                                doc.status(transaction_document_status::IN_TXN_COMMITTED);
                            }
                        } else {
                            doc.status(transaction_document_status::IN_TXN_OTHER);
                            if (doc.content() == nullptr) {
                                throw std::runtime_error(std::string("not found"));
                            }
                        }
                    } else {
                        doc.content(doc.links().staged_content());
                        doc.status(transaction_document_status::OWN_WRITE);
                    }
                }
            }
        }
        LOG(txctx_, trace) << "completed get of " << doc;
        out.emplace(doc);
        return out;
    }
    LOG(txctx_, warning) << "got error while getting doc " << id << ": " << lcb_strerror_short(res.rc);
    return out;
}

tx::transaction_document tx::attempt_context::replace(couchbase::collection *collection, const tx::transaction_document &document,
                                                      const folly::dynamic &content)
{
    init_atr_if_needed(collection, document.id());

    if (staged_mutations_.empty()) {
        std::string prefix(ATR_FIELD_ATTEMPTS + "." + id_ + ".");
        const result &res = collection->mutate_in(
            atr_id_.value(),
            {
                mutate_in_spec::insert(prefix + ATR_FIELD_STATUS, attempt_state_name(attempt_state::PENDING)).xattr().create_path(),
                mutate_in_spec::insert(prefix + ATR_FIELD_START_TIMESTAMP, "${Mutation.CAS}").xattr().expand_macro(),
                mutate_in_spec::insert(prefix + ATR_FIELD_EXPIRES_AFTER_MSECS, 15).xattr(),
                mutate_in_spec::fulldoc_upsert(folly::dynamic::object()),
            },
            durability(config_));
        if (res.rc != LCB_SUCCESS) {
            throw std::runtime_error(std::string("failed to set ATR to pending state: ") + lcb_strerror_short(res.rc));
        }
    }

    LOG(txctx_, trace) << "replacing doc " << document.id();
    const result &res = collection->mutate_in(document.id(),
                                              {
                                                  mutate_in_spec::upsert(STAGED_VERSION, id_).xattr().create_path(),
                                                  mutate_in_spec::upsert(ATR_ID, atr_id_.value()).xattr(),
                                                  mutate_in_spec::upsert(STAGED_DATA, content).xattr(),
                                                  mutate_in_spec::upsert(ATR_BUCKET_NAME, collection->bucket_name()).xattr(),
                                                  mutate_in_spec::upsert(ATR_SCOPE_NAME, collection->scope()).xattr(),
                                                  mutate_in_spec::upsert(ATR_COLL_NAME, collection->name()).xattr(),
                                              },
                                              durability(config_));

    if (res.rc == LCB_SUCCESS) {
        transaction_document out(
            *collection, document.id(), document.content(), res.cas, transaction_document_status::NORMAL,
            transaction_links(atr_id_.value(), collection->bucket_name(), collection->scope(), collection->name(), content, id_));
        staged_mutations_.add(staged_mutation(out, content, staged_mutation_type::REPLACE));
        return out;
    }
    throw std::runtime_error(std::string("failed to replace the document: ") + lcb_strerror_short(res.rc));
}

tx::transaction_document tx::attempt_context::insert(couchbase::collection *collection, const std::string &id,
                                                     const folly::dynamic &content)
{
    init_atr_if_needed(collection, id);

    if (staged_mutations_.empty()) {
        std::string prefix(ATR_FIELD_ATTEMPTS + "." + id_ + ".");
        collection->mutate_in(
            id,
            {
                mutate_in_spec::insert(prefix + ATR_FIELD_STATUS, attempt_state_name(attempt_state::PENDING)).xattr().create_path(),
                mutate_in_spec::insert(prefix + ATR_FIELD_START_TIMESTAMP, "${Mutation.CAS}").xattr().expand_macro(),
                mutate_in_spec::insert(prefix + ATR_FIELD_EXPIRES_AFTER_MSECS, 15).xattr(),
                mutate_in_spec::fulldoc_upsert(folly::dynamic::object()),
            },
            durability(config_));
    }

    LOG(txctx_, trace) << "inserting doc " << id;
    const result &res = collection->mutate_in(id,
                                              {
                                                  mutate_in_spec::upsert(STAGED_VERSION, id_).xattr().create_path(),
                                                  mutate_in_spec::insert(ATR_ID, atr_id_.value()).xattr(),
                                                  mutate_in_spec::insert(STAGED_DATA, content).xattr(),
                                                  mutate_in_spec::insert(ATR_BUCKET_NAME, collection->bucket_name()).xattr(),
                                                  mutate_in_spec::insert(ATR_SCOPE_NAME, collection->scope()).xattr(),
                                                  mutate_in_spec::insert(ATR_COLL_NAME, collection->name()).xattr(),
                                                  mutate_in_spec::fulldoc_insert(folly::dynamic::object()),
                                              },
                                              durability(config_));
    if (res.rc == LCB_SUCCESS) {
        transaction_document out(
            *collection, id, content, res.cas, transaction_document_status::NORMAL,
            transaction_links(atr_id_.value(), collection->bucket_name(), collection->scope(), collection->name(), content, id_));
        staged_mutations_.add(staged_mutation(out, content, staged_mutation_type::INSERT));
        return out;
    }
    throw std::runtime_error(std::string("failed to insert the document: ") + lcb_strerror_short(res.rc));
}

void tx::attempt_context::remove(couchbase::collection *collection, tx::transaction_document &document)
{
    init_atr_if_needed(collection, document.id());

    if (staged_mutations_.empty()) {
        std::string prefix(ATR_FIELD_ATTEMPTS + "." + id_ + ".");
        collection->mutate_in(
            document.id(),
            {
                mutate_in_spec::insert(prefix + ATR_FIELD_STATUS, attempt_state_name(attempt_state::PENDING)).xattr().create_path(),
                mutate_in_spec::insert(prefix + ATR_FIELD_START_TIMESTAMP, "${Mutation.CAS}").xattr().expand_macro(),
                mutate_in_spec::insert(prefix + ATR_FIELD_EXPIRES_AFTER_MSECS, 15).xattr(),
                mutate_in_spec::fulldoc_upsert(folly::dynamic::object()),
            },
            durability(config_));
    }

    LOG(txctx_, trace) << "removing doc " << document.id();
    const result &res = collection->mutate_in(document.id(),
                                              {
                                                  mutate_in_spec::upsert(STAGED_VERSION, id_).xattr().create_path(),
                                                  mutate_in_spec::upsert(ATR_ID, atr_id_.value()).xattr(),
                                                  mutate_in_spec::upsert(STAGED_DATA, STAGED_DATA_REMOVED_VALUE).xattr(),
                                                  mutate_in_spec::upsert(ATR_BUCKET_NAME, collection->bucket_name()).xattr(),
                                                  mutate_in_spec::upsert(ATR_SCOPE_NAME, collection->scope()).xattr(),
                                                  mutate_in_spec::upsert(ATR_COLL_NAME, collection->name()).xattr(),
                                              },
                                              durability(config_));
    if (res.rc == LCB_SUCCESS) {
        document.cas(res.cas);
        staged_mutations_.add(staged_mutation(document, "", staged_mutation_type::REMOVE));
        return;
    }
    throw std::runtime_error(std::string("failed to remove the document: ") + lcb_strerror_short(res.rc));
}

void tx::attempt_context::commit()
{
    std::string prefix(ATR_FIELD_ATTEMPTS + "." + id_ + ".");
    std::vector<mutate_in_spec> specs({
        mutate_in_spec::upsert(prefix + ATR_FIELD_STATUS, attempt_state_name(attempt_state::COMMITTED)).xattr(),
        mutate_in_spec::upsert(prefix + ATR_FIELD_START_COMMIT, "${Mutation.CAS}").xattr().expand_macro(),
    });
    staged_mutations_.extract_to(prefix, specs);
    const result &res = atr_collection_->mutate_in(atr_id_.value(), specs);
    if (res.rc == LCB_SUCCESS) {
        std::vector<transaction_document> docs;
        staged_mutations_.commit();
        is_done_ = true;
        state_ = attempt_state::COMMITTED;
    } else {
        throw std::runtime_error(std::string("failed to commit transaction: ") + id_ + ": " + lcb_strerror_short(res.rc));
    }
}

const std::string &couchbase::transactions::attempt_context::id()
{
    return id_;
}
