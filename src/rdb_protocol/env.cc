// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "rdb_protocol/env.hpp"

#include "concurrency/cross_thread_watchable.hpp"
#include "extproc/js_runner.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/term_walker.hpp"

#include "debug.hpp"

// This is a totally arbitrary constant limiting the size of the regex cache.  1000
// was chosen out of a hat; if you have a good argument for it being something else
// (apart from cache line concerns, which are irrelevant due to the implementation)
// you're probably right.
const size_t LRU_CACHE_SIZE = 1000;

namespace ql {

void env_t::set_eval_callback(eval_callback_t *callback) {
    eval_callback_ = callback;
}

void env_t::do_eval_callback() {
    if (eval_callback_ != NULL) {
        eval_callback_->eval_callback();
    }
}

profile_bool_t env_t::profile() const {
    return trace != nullptr ? profile_bool_t::PROFILE : profile_bool_t::DONT_PROFILE;
}

reql_cluster_interface_t *env_t::reql_cluster_interface() {
    r_sanity_check(rdb_ctx_ != NULL);
    return rdb_ctx_->cluster_interface;
}

std::string env_t::get_reql_http_proxy() {
    r_sanity_check(rdb_ctx_ != NULL);
    return rdb_ctx_->reql_http_proxy;
}

extproc_pool_t *env_t::get_extproc_pool() {
    assert_thread();
    r_sanity_check(rdb_ctx_ != NULL);
    r_sanity_check(rdb_ctx_->extproc_pool != NULL);
    return rdb_ctx_->extproc_pool;
}

scoped_ptr_t<profile::trace_t> maybe_make_profile_trace(profile_bool_t profile) {
    return profile == profile_bool_t::PROFILE
        ? make_scoped<profile::trace_t>()
        : scoped_ptr_t<profile::trace_t>();
}

env_t::env_t(rdb_context_t *ctx,
             return_empty_normal_batches_t _return_empty_normal_batches,
             signal_t *_interruptor,
             serializable_env_t s_env,
             profile::trace_t *_trace)
    : serializable_(std::move(s_env)),
      limits_(from_optargs(ctx, _interruptor, &serializable_.global_optargs,
                           serializable_.deterministic_time)),
      reql_version_(reql_version_t::LATEST),
      regex_cache_(LRU_CACHE_SIZE),
      return_empty_normal_batches(_return_empty_normal_batches),
      interruptor(_interruptor),
      trace(_trace),
      evals_since_yield_(0),
      rdb_ctx_(ctx),
      eval_callback_(NULL) {
    rassert(ctx != NULL);
    rassert(interruptor != NULL);
}

env_t::env_t(rdb_context_t *ctx,
             return_empty_normal_batches_t _return_empty_normal_batches,
             signal_t *_interruptor,
             global_optargs_t _global_optargs,
             auth::user_context_t _user_context,
             datum_t _deterministic_time,
             profile::trace_t *_trace) :
    env_t(ctx,
          _return_empty_normal_batches,
          _interruptor,
          serializable_env_t{
                  std::move(_global_optargs),
                  std::move(_user_context),
                  std::move(_deterministic_time)},
          _trace) { }

// Used in constructing the env for rdb_update_single_sindex and many unit tests.
env_t::env_t(signal_t *_interruptor,
             return_empty_normal_batches_t _return_empty_normal_batches,
             reql_version_t _reql_version)
    : serializable_{
        global_optargs_t(),
        auth::user_context_t(auth::permissions_t(tribool::False, tribool::False, tribool::False, tribool::False)),
        datum_t()},
      reql_version_(_reql_version),
      regex_cache_(LRU_CACHE_SIZE),
      return_empty_normal_batches(_return_empty_normal_batches),
      interruptor(_interruptor),
      trace(NULL),
      evals_since_yield_(0),
      rdb_ctx_(NULL),
      eval_callback_(NULL) {
    rassert(interruptor != NULL);
}

env_t::~env_t() { }

void env_t::maybe_yield() {
    if (++evals_since_yield_ > EVALS_BEFORE_YIELD) {
        evals_since_yield_ = 0;
        coro_t::yield();
    }
}

} // namespace ql
