/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_element.h
 * @brief Tree elements
 *
 * @since New in 1.10.
 */

#ifndef SVN_ELEMENT_H
#define SVN_ELEMENT_H

#include <apr_pools.h>

#include "svn_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/** A location in a committed revision.
 *
 * @a rev shall not be #SVN_INVALID_REVNUM unless the interface using this
 * type specifically allows it and defines its meaning. */
typedef struct svn_pathrev_t
{
  svn_revnum_t rev;
  const char *relpath;
} svn_pathrev_t;

/**
 */
typedef struct svn_element_branch_ref_t
{
  svn_revnum_t rev;
  const char *branch_id;
  int eid;
} svn_element_branch_ref_t;

/* Return a duplicate of OLD, allocated in RESULT_POOL. */
svn_pathrev_t
svn_pathrev_dup(svn_pathrev_t old,
                apr_pool_t *result_pool);

/* Return true iff PEG_PATH1 and PEG_PATH2 are both the same location.
 */
svn_boolean_t
svn_pathrev_equal(svn_pathrev_t *peg_path1,
                  svn_pathrev_t *peg_path2);

/** Versioned payload of an element, excluding tree structure information.
 *
 * This specifies the properties and the text of a file or target of a
 * symlink, directly, or by reference to an existing committed element, or
 * by a delta against such a reference payload.
 *
 * ### An idea: If the sender and receiver agree, the payload for an element
 *     may be specified as "null" to designate that the payload is not
 *     available. For example, when a client performing a WC update has
 *     no read authorization for a given path, the server may send null
 *     payload and the client may record an 'absent' WC node. (This
 *     would not make sense in a commit.)
 */
typedef struct svn_element_payload_t svn_element_payload_t;

/*
 * ========================================================================
 * Element Payload Interface
 * ========================================================================
 *
 * @defgroup svn_element_payload Element payload interface
 * @{
 */

/** Versioned payload of a node, excluding tree structure information.
 *
 * Payload is described by setting fields in one of the following ways.
 * Other fields SHOULD be null (or equivalent).
 *
 *   by reference:  (kind=unknown, ref)
 *   dir:           (kind=dir, props)
 *   file:          (kind=file, props, text)
 *   symlink:       (kind=symlink, props, target)
 *
 * ### Idea for the future: Specify payload as an (optional) reference
 *     plus (optional) overrides or deltas against the reference?
 */
struct svn_element_payload_t
{
  /* Is this a subbranch-root element, in other words a link to a nested
   * branch? If so, all other fields are irrelevant. */
  svn_boolean_t is_subbranch_root;

  /* The node kind for this payload: dir, file, symlink, or unknown. */
  svn_node_kind_t kind;

  /* Reference an existing, committed payload. (Use with kind=unknown if
   * there is no content in props/text/targe fields.)
   * The 'null' value is (SVN_INVALID_REVNUM, NULL, *). */
  svn_element_branch_ref_t branch_ref;

  /* The pool in which the payload's content is allocated. Used when
   * resolving (populating the props/text/target in) a payload that was
   * originally defined by reference. */
  apr_pool_t *pool;

  /* Properties (for kind != unknown).
   * Maps (const char *) name -> (svn_string_t) value.
   * An empty hash means no properties. (SHOULD NOT be NULL.)
   * ### Presently NULL means 'no change' in some contexts. */
  apr_hash_t *props;

  /* File text (for kind=file; otherwise SHOULD be NULL). */
  svn_stringbuf_t *text;

  /* Symlink target (for kind=symlink; otherwise SHOULD be NULL). */
  const char *target;

};

/* Return true iff PAYLOAD satisfies all its invariants.
 */
svn_boolean_t
svn_element_payload_invariants(const svn_element_payload_t *payload);

/** Duplicate a node-payload @a old into @a result_pool.
 */
svn_element_payload_t *
svn_element_payload_dup(const svn_element_payload_t *old,
                        apr_pool_t *result_pool);

/* Return true iff the payload of LEFT is identical to that of RIGHT.
 * References are not supported. Node kind 'unknown' is not supported.
 */
svn_boolean_t
svn_element_payload_equal(const svn_element_payload_t *left,
                          const svn_element_payload_t *right,
                          apr_pool_t *scratch_pool);

/** Create a new node-payload object for a subbranch-root (link to a
 * nested branch).
 *
 * Allocate the result in @a result_pool.
 */
svn_element_payload_t *
svn_element_payload_create_subbranch(apr_pool_t *result_pool);

/** Create a new node-payload object by reference to an existing payload.
 *
 * Set the node kind to 'unknown'.
 *
 * Allocate the result in @a result_pool.
 */
svn_element_payload_t *
svn_element_payload_create_ref(svn_revnum_t rev,
                               const char *branch_id,
                               int eid,
                               apr_pool_t *result_pool);

/** Create a new node-payload object for a directory node.
 *
 * Allocate the result in @a result_pool.
 */
svn_element_payload_t *
svn_element_payload_create_dir(apr_hash_t *props,
                               apr_pool_t *result_pool);

/** Create a new node-payload object for a file node.
 *
 * Allocate the result in @a result_pool.
 */
svn_element_payload_t *
svn_element_payload_create_file(apr_hash_t *props,
                                svn_stringbuf_t *text,
                                apr_pool_t *result_pool);

/** Create a new node-payload object for a symlink node.
 *
 * Allocate the result in @a result_pool.
 */
svn_element_payload_t *
svn_element_payload_create_symlink(apr_hash_t *props,
                                   const char *target,
                                   apr_pool_t *result_pool);

/** @} */


/*
 * ========================================================================
 * Element-Revision Content
 * ========================================================================
 *
 * @defgroup svn_el_rev_content Element-Revision Content
 * @{
 */

/* The content (parent, name and payload) of an element-revision.
 * In other words, an el-rev node in a (mixed-rev) directory-tree.
 */
typedef struct svn_element_content_t
{
  /* eid of the parent element, or -1 if this is the root element */
  int parent_eid;
  /* element name, or "" for root element; never null */
  const char *name;
  /* payload (kind, props, text, ...) */
  svn_element_payload_t *payload;

} svn_element_content_t;

/* Return a new content object constructed with deep copies of PARENT_EID,
 * NAME and PAYLOAD, allocated in RESULT_POOL.
 */
svn_element_content_t *
svn_element_content_create(int parent_eid,
                           const char *name,
                           const svn_element_payload_t *payload,
                           apr_pool_t *result_pool);

/* Return a deep copy of OLD, allocated in RESULT_POOL.
 */
svn_element_content_t *
svn_element_content_dup(const svn_element_content_t *old,
                        apr_pool_t *result_pool);

/* Return TRUE iff CONTENT_LEFT is the same as CONTENT_RIGHT. */
svn_boolean_t
svn_element_content_equal(const svn_element_content_t *content_left,
                          const svn_element_content_t *content_right,
                          apr_pool_t *scratch_pool);

/** @} */


/*
 * ========================================================================
 * Element Tree
 * ========================================================================
 *
 * The elements in an Element Tree do not necessarily form a single,
 * complete tree at all times.
 *
 * @defgroup svn_element_tree Element Tree
 * @{
 */

/* A (sub)tree of elements.
 *
 * An element tree is described by the content of element ROOT_EID in E_MAP,
 * and its children (as determined by their parent links) and their names
 * and their content recursively. For the element ROOT_EID itself, only
 * its content is relevant; its parent and name are to be ignored.
 *
 * E_MAP may also contain entries that are not part of the subtree. Thus,
 * to select a sub-subtree, it is only necessary to change ROOT_EID.
 *
 * The EIDs used in here may be considered either as global EIDs (known to
 * the repo), or as local stand-alone EIDs (in their own local name-space),
 * according to the context.
 */
typedef struct svn_element_tree_t
{
  /* EID -> svn_branch_el_rev_content_t mapping. */
  apr_hash_t *e_map;

  /* Subtree root EID. (ROOT_EID must be an existing key in E_MAP.) */
  int root_eid;

} svn_element_tree_t;

/* Create an element tree object.
 *
 * The result contains a *shallow* copy of E_MAP, or a new empty mapping
 * if E_MAP is null.
 */
svn_element_tree_t *
svn_element_tree_create(apr_hash_t *e_map,
                        int root_eid,
                        apr_pool_t *result_pool);

svn_element_content_t *
svn_element_tree_get(const svn_element_tree_t *tree,
                     int eid);

svn_error_t *
svn_element_tree_set(svn_element_tree_t *tree,
                     int eid,
                     svn_element_content_t *element);

/* Purge entries from E_MAP that don't connect, via parent directory hierarchy,
 * to ROOT_EID. In other words, remove elements that have been implicitly
 * deleted.
 *
 * ROOT_EID must be present in E_MAP.
 *
 * ### Does not detect cycles: current implementation will not purge a cycle
 *     that is disconnected from ROOT_EID. This could be a problem.
 */
void
svn_element_tree_purge_orphans(apr_hash_t *e_map,
                               int root_eid,
                               apr_pool_t *scratch_pool);

/* Return the subtree-relative path of element EID in TREE.
 *
 * If the element EID does not currently exist in TREE, return NULL.
 *
 * ### TODO: Clarify sequencing requirements.
 */
const char *
svn_element_tree_get_path_by_eid(const svn_element_tree_t *tree,
                                 int eid,
                                 apr_pool_t *result_pool);

/** @} */


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_ELEMENT_H */