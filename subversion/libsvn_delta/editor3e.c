/*
 * editor3e.c :  editing trees of versioned resources
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <apr_pools.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_iter.h"

#include "private/svn_editor3e.h"
#include "svn_private_config.h"

#ifdef SVN_DEBUG
/* This enables runtime checks of the editor API constraints.  This may
   introduce additional memory and runtime overhead, and should not be used
   in production builds. */
#define ENABLE_ORDERING_CHECK
#endif


struct svn_editor3_t
{
  void *baton;

  /* Standard cancellation function. Called before each callback.  */
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  /* The callback functions.  */
  svn_editor3_cb_funcs_t funcs;

  /* This pool is used as the scratch_pool for all callbacks.  */
  apr_pool_t *scratch_pool;

#ifdef ENABLE_ORDERING_CHECK
  svn_boolean_t within_callback;
  svn_boolean_t finished;
  apr_pool_t *state_pool;
#endif
};


#ifdef ENABLE_ORDERING_CHECK

#define START_CALLBACK(editor)                       \
  do {                                               \
    svn_editor3_t *editor__tmp_e = (editor);          \
    SVN_ERR_ASSERT(!editor__tmp_e->within_callback); \
    editor__tmp_e->within_callback = TRUE;           \
  } while (0)
#define END_CALLBACK(editor) ((editor)->within_callback = FALSE)

#define MARK_FINISHED(editor) ((editor)->finished = TRUE)
#define SHOULD_NOT_BE_FINISHED(editor)  SVN_ERR_ASSERT(!(editor)->finished)

#else

#define START_CALLBACK(editor)  /* empty */
#define END_CALLBACK(editor)  /* empty */

#define MARK_FINISHED(editor)  /* empty */
#define SHOULD_NOT_BE_FINISHED(editor)  /* empty */

#endif /* ENABLE_ORDERING_CHECK */


svn_editor3_t *
svn_editor3_create(const svn_editor3_cb_funcs_t *editor_funcs,
                   void *editor_baton,
                   svn_cancel_func_t cancel_func,
                   void *cancel_baton,
                   apr_pool_t *result_pool)
{
  svn_editor3_t *editor = apr_pcalloc(result_pool, sizeof(*editor));

  editor->funcs = *editor_funcs;
  editor->baton = editor_baton;
  editor->cancel_func = cancel_func;
  editor->cancel_baton = cancel_baton;
  editor->scratch_pool = svn_pool_create(result_pool);

#ifdef ENABLE_ORDERING_CHECK
  editor->within_callback = FALSE;
  editor->finished = FALSE;
  editor->state_pool = result_pool;
#endif

  return editor;
}


void *
svn_editor3__get_baton(const svn_editor3_t *editor)
{
  return editor->baton;
}


static svn_error_t *
check_cancel(svn_editor3_t *editor)
{
  svn_error_t *err = NULL;

  if (editor->cancel_func)
    {
      START_CALLBACK(editor);
      err = editor->cancel_func(editor->cancel_baton);
      END_CALLBACK(editor);
    }

  return svn_error_trace(err);
}

/* Do everything that is common to calling any callback.
 *
 * CB is the name of the callback method, e.g. "cb_add".
 * ARG_LIST is the callback-specific arguments prefixed by the number of
 * these arguments, in the form "3(arg1, arg2, arg3)".
 */
#define DO_CALLBACK(editor, cb, arg_list)                               \
  {                                                                     \
    SVN_ERR(check_cancel(editor));                                      \
    if ((editor)->funcs.cb)                                             \
      {                                                                 \
        svn_error_t *_do_cb_err;                                        \
        START_CALLBACK(editor);                                         \
        _do_cb_err = (editor)->funcs.cb((editor)->baton,                \
                                        ARGS ## arg_list                \
                                        (editor)->scratch_pool);        \
        END_CALLBACK(editor);                                           \
        svn_pool_clear((editor)->scratch_pool);                         \
        SVN_ERR(_do_cb_err);                                            \
      }                                                                 \
  }
#define ARGS0()
#define ARGS1(a1)                             a1,
#define ARGS2(a1, a2)                         a1, a2,
#define ARGS3(a1, a2, a3)                     a1, a2, a3,
#define ARGS4(a1, a2, a3, a4)                 a1, a2, a3, a4,
#define ARGS5(a1, a2, a3, a4, a5)             a1, a2, a3, a4, a5,
#define ARGS6(a1, a2, a3, a4, a5, a6)         a1, a2, a3, a4, a5, a6,
#define ARGS7(a1, a2, a3, a4, a5, a6, a7)     a1, a2, a3, a4, a5, a6, a7,
#define ARGS8(a1, a2, a3, a4, a5, a6, a7, a8) a1, a2, a3, a4, a5, a6, a7, a8,


/*
 * ========================================================================
 * Editor for Commit (independent per-element changes; element-id addressing)
 * ========================================================================
 */

#define VALID_NODE_KIND(kind) ((kind) != svn_node_unknown && (kind) != svn_node_none)
#define VALID_EID(eid) ((eid) != -1)
#define VALID_NAME(name) ((name) && (name)[0] && svn_relpath_is_canonical(name))
#define VALID_PAYLOAD(payload) svn_element_payload_invariants(payload)
#define VALID_EL_REV_ID(el_rev) (el_rev && el_rev->branch && VALID_EID(el_rev->eid))
#define VALID_REV_BID_EID(rbe) (rbe && rbe->bid && VALID_EID(rbe->eid))

#define VERIFY(method, expr) \
  if (! (expr)) \
    return svn_error_createf(SVN_ERR_BRANCHING, NULL, \
                             _("svn_editor3_%s: validation (%s) failed"), \
                             #method, #expr)

svn_error_t *
svn_editor3_new_eid(svn_editor3_t *editor,
                    svn_branch_eid_t *eid_p)
{
  int eid = -1;

  DO_CALLBACK(editor, cb_new_eid,
              1(&eid));

  SVN_ERR_ASSERT(VALID_EID(eid));

  /* We allow the output pointer to be null, here, so that implementations
     may assume their output pointer is non-null. */
  if (eid_p)
    *eid_p = eid;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_open_branch(svn_editor3_t *editor,
                        const char **new_branch_id_p,
                        svn_branch_rev_bid_t *predecessor,
                        const char *outer_branch_id,
                        int outer_eid,
                        int root_eid,
                        apr_pool_t *result_pool)
{
  const char *new_branch_id;

  SVN_ERR_ASSERT(VALID_EID(outer_eid));
  SVN_ERR_ASSERT(VALID_EID(root_eid));

  DO_CALLBACK(editor, cb_open_branch,
              6(&new_branch_id,
                predecessor, outer_branch_id, outer_eid, root_eid,
                result_pool));

  /* We allow the output pointer to be null, here, so that implementations
     may assume their output pointer is non-null. */
  if (new_branch_id_p)
    *new_branch_id_p = new_branch_id;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_branch(svn_editor3_t *editor,
                   const char **new_branch_id_p,
                   svn_branch_rev_bid_eid_t *from,
                   const char *outer_branch_id,
                   int outer_eid,
                   apr_pool_t *result_pool)
{
  const char *new_branch_id;

  SVN_ERR_ASSERT(VALID_EID(outer_eid));

  DO_CALLBACK(editor, cb_branch,
              5(&new_branch_id, from, outer_branch_id, outer_eid,
                result_pool));

  /* We allow the output pointer to be null, here, so that implementations
     may assume their output pointer is non-null. */
  if (new_branch_id_p)
    *new_branch_id_p = new_branch_id;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_alter(svn_editor3_t *editor,
                  const char *branch_id,
                  svn_branch_eid_t eid,
                  svn_branch_eid_t new_parent_eid,
                  const char *new_name,
                  const svn_element_payload_t *new_payload)
{
  SVN_ERR_ASSERT(VALID_EID(eid));
  /*SVN_ERR_ASSERT(eid == branch->root_eid ? new_parent_eid == -1
                                         : VALID_EID(new_parent_eid));*/
  /*SVN_ERR_ASSERT(eid == branch->root_eid ? *new_name == '\0'
                                         : VALID_NAME(new_name));*/
  SVN_ERR_ASSERT(VALID_PAYLOAD(new_payload));
  VERIFY(alter, new_parent_eid != eid);

  DO_CALLBACK(editor, cb_alter,
              5(branch_id, eid,
                new_parent_eid, new_name,
                new_payload));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_copy_one(svn_editor3_t *editor,
                     const svn_branch_rev_bid_eid_t *src_el_rev,
                     const char *branch_id,
                     svn_branch_eid_t local_eid,
                     svn_branch_eid_t new_parent_eid,
                     const char *new_name,
                     const svn_element_payload_t *new_payload)
{
  SVN_ERR_ASSERT(VALID_EID(local_eid));
  SVN_ERR_ASSERT(VALID_REV_BID_EID(src_el_rev));
  SVN_ERR_ASSERT(VALID_EID(new_parent_eid));
  SVN_ERR_ASSERT(VALID_NAME(new_name));
  SVN_ERR_ASSERT(VALID_PAYLOAD(new_payload));
  /* TODO: verify source element exists (in a committed rev) */

  DO_CALLBACK(editor, cb_copy_one,
              6(src_el_rev,
                branch_id, local_eid,
                new_parent_eid, new_name,
                new_payload));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_copy_tree(svn_editor3_t *editor,
                      const svn_branch_rev_bid_eid_t *src_el_rev,
                      const char *branch_id,
                      svn_branch_eid_t new_parent_eid,
                      const char *new_name)
{
  SVN_ERR_ASSERT(VALID_REV_BID_EID(src_el_rev));
  SVN_ERR_ASSERT(VALID_EID(new_parent_eid));
  SVN_ERR_ASSERT(VALID_NAME(new_name));
  /* TODO: verify source element exists (in a committed rev) */

  DO_CALLBACK(editor, cb_copy_tree,
              4(src_el_rev,
                branch_id, new_parent_eid, new_name));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_delete(svn_editor3_t *editor,
                   const char *branch_id,
                   svn_branch_eid_t eid)
{
  SVN_ERR_ASSERT(VALID_EID(eid));
  /*SVN_ERR_ASSERT(eid != branch->root_eid);*/
  /* TODO: verify this element exists (in initial state) */

  DO_CALLBACK(editor, cb_delete,
              2(branch_id, eid));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_sequence_point(svn_editor3_t *editor)
{
  SHOULD_NOT_BE_FINISHED(editor);

  DO_CALLBACK(editor, cb_sequence_point,
              0());

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_complete(svn_editor3_t *editor)
{
  SHOULD_NOT_BE_FINISHED(editor);

  DO_CALLBACK(editor, cb_complete,
              0());

  MARK_FINISHED(editor);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3_abort(svn_editor3_t *editor)
{
  SHOULD_NOT_BE_FINISHED(editor);

  DO_CALLBACK(editor, cb_abort,
              0());

  MARK_FINISHED(editor);

  return SVN_NO_ERROR;
}


#ifdef SVN_DEBUG

/*
 * ===================================================================
 * A wrapper editor that forwards calls through to a wrapped editor
 * while printing a diagnostic trace of the calls.
 * ===================================================================
 */

typedef struct wrapper_baton_t
{
  svn_editor3_t *wrapped_editor;

  /* debug printing stream */
  svn_stream_t *debug_stream;
  /* debug printing prefix*/
  const char *prefix;

} wrapper_baton_t;

/* Print the variable arguments, formatted with FMT like with 'printf',
 * to the stream EB->debug_stream, prefixed with EB->prefix. */
static void
dbg(wrapper_baton_t *eb,
    apr_pool_t *scratch_pool,
    const char *fmt,
    ...)
{
  const char *message;
  va_list ap;

  va_start(ap, fmt);
  message = apr_pvsprintf(scratch_pool, fmt, ap);
  va_end(ap);

  if (eb->prefix)
    svn_error_clear(svn_stream_puts(eb->debug_stream, eb->prefix));
  svn_error_clear(svn_stream_puts(eb->debug_stream, message));
  svn_error_clear(svn_stream_puts(eb->debug_stream, "\n"));
}

/* Return a human-readable string representation of EL_REV. */
static const char *
rev_bid_eid_str(const svn_branch_rev_bid_eid_t *el_rev,
                apr_pool_t *result_pool)
{
  return apr_psprintf(result_pool, "r%ldb%se%d",
                      el_rev->rev, el_rev->bid, el_rev->eid);
}

/* Return a human-readable string representation of EID. */
static const char *
eid_str(svn_branch_eid_t eid,
         apr_pool_t *result_pool)
{
  return apr_psprintf(result_pool, "%d", eid);
}

static svn_error_t *
wrap_new_eid(void *baton,
             svn_branch_eid_t *eid_p,
             apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  SVN_ERR(svn_editor3_new_eid(eb->wrapped_editor,
                              eid_p));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_open_branch(void *baton,
                 const char **new_branch_id_p,
                 svn_branch_rev_bid_t *predecessor,
                 const char *outer_branch_id,
                 int outer_eid,
                 int root_eid,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  /*dbg(eb, scratch_pool, "%s : open_branch(...)",
      eid_str(eid, scratch_pool), ...);*/
  SVN_ERR(svn_editor3_open_branch(eb->wrapped_editor,
                                  new_branch_id_p,
                                  predecessor,
                                  outer_branch_id, outer_eid, root_eid,
                                  result_pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_branch(void *baton,
            const char **new_branch_id_p,
            svn_branch_rev_bid_eid_t *from,
            const char *outer_branch_id,
            int outer_eid,
            apr_pool_t *result_pool,
            apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  /*dbg(eb, scratch_pool, "%s : open_branch(...)",
      eid_str(eid, scratch_pool), ...);*/
  SVN_ERR(svn_editor3_branch(eb->wrapped_editor,
                             new_branch_id_p,
                             from, outer_branch_id, outer_eid,
                             result_pool));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_alter(void *baton,
           const char *branch_id,
           svn_branch_eid_t eid,
           svn_branch_eid_t new_parent_eid,
           const char *new_name,
           const svn_element_payload_t *new_payload,
           apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  dbg(eb, scratch_pool, "%s : alter(p=%s, n=%s, k=%s)",
      eid_str(eid, scratch_pool),
      eid_str(new_parent_eid, scratch_pool), new_name,
      (!new_payload->is_subbranch_root) ? svn_node_kind_to_word(new_payload->kind) : "subbranch");
  SVN_ERR(svn_editor3_alter(eb->wrapped_editor,
                            branch_id, eid,
                            new_parent_eid, new_name, new_payload));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_copy_one(void *baton,
              const svn_branch_rev_bid_eid_t *src_el_rev,
              const char *branch_id,
              svn_branch_eid_t local_eid,
              svn_branch_eid_t new_parent_eid,
              const char *new_name,
              const svn_element_payload_t *new_payload,
              apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  dbg(eb, scratch_pool, "%s : copy_one(f=%s, p=%s, n=%s, c=...)",
      eid_str(local_eid, scratch_pool),
      rev_bid_eid_str(src_el_rev, scratch_pool),
      eid_str(new_parent_eid, scratch_pool), new_name);
  SVN_ERR(svn_editor3_copy_one(eb->wrapped_editor,
                               src_el_rev,
                               branch_id, local_eid,
                               new_parent_eid, new_name, new_payload));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_copy_tree(void *baton,
               const svn_branch_rev_bid_eid_t *src_el_rev,
               const char *branch_id,
               svn_branch_eid_t new_parent_eid,
               const char *new_name,
               apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  dbg(eb, scratch_pool, "... : copy_tree(f=%s, p=%s, n=%s)",
      rev_bid_eid_str(src_el_rev, scratch_pool),
      eid_str(new_parent_eid, scratch_pool), new_name);
  SVN_ERR(svn_editor3_copy_tree(eb->wrapped_editor,
                                src_el_rev,
                                branch_id, new_parent_eid, new_name));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_delete(void *baton,
            const char *branch_id,
            svn_branch_eid_t eid,
            apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  dbg(eb, scratch_pool, "%s : delete()",
      eid_str(eid, scratch_pool));
  SVN_ERR(svn_editor3_delete(eb->wrapped_editor,
                             branch_id, eid));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_sequence_point(void *baton,
                    apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  dbg(eb, scratch_pool, "sequence_point()");
  SVN_ERR(svn_editor3_sequence_point(eb->wrapped_editor));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_complete(void *baton,
              apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  dbg(eb, scratch_pool, "complete()");
  SVN_ERR(svn_editor3_complete(eb->wrapped_editor));
  return SVN_NO_ERROR;
}

static svn_error_t *
wrap_abort(void *baton,
           apr_pool_t *scratch_pool)
{
  wrapper_baton_t *eb = baton;

  dbg(eb, scratch_pool, "abort()");
  SVN_ERR(svn_editor3_abort(eb->wrapped_editor));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_editor3__get_debug_editor(svn_editor3_t **editor_p,
                              svn_editor3_t *wrapped_editor,
                              apr_pool_t *result_pool)
{
  static const svn_editor3_cb_funcs_t wrapper_funcs = {
    wrap_new_eid,
    wrap_open_branch,
    wrap_branch,
    wrap_alter,
    wrap_copy_one,
    wrap_copy_tree,
    wrap_delete,
    wrap_sequence_point,
    wrap_complete,
    wrap_abort
  };
  wrapper_baton_t *eb = apr_palloc(result_pool, sizeof(*eb));

  eb->wrapped_editor = wrapped_editor;

  /* set up for diagnostic printing */
  {
    apr_file_t *errfp;
    apr_status_t apr_err = apr_file_open_stdout(&errfp, result_pool);

    if (apr_err)
      return svn_error_wrap_apr(apr_err, "Failed to open debug output stream");

    eb->debug_stream = svn_stream_from_aprfile2(errfp, TRUE, result_pool);
    eb->prefix = apr_pstrdup(result_pool, "DBG: ");
  }

  *editor_p = svn_editor3_create(&wrapper_funcs, eb,
                                 NULL, NULL, /* cancellation */
                                 result_pool);

  return SVN_NO_ERROR;
}
#endif
